#!/usr/bin/env python3
"""Check every air.* arithmetic operation the corpus uses against a reference.

The corpus exercises each operation only incidentally: a shader that calls
air.fast_round once and a hundred other things tells us the module translated,
not that the rounding is right. One of these was wrong for a week -- fast_round
was lowered to nir_fround_even, which is rint, and the two differ at every
half-way value -- and nothing in the 456-module corpus could have shown it,
because no module's output was ever compared with a number computed any other
way.

So the operations are tested one at a time, on inputs chosen to break them:

  - air2nir --list over the corpus gives the declarations that are actually
    used (78 arithmetic ones today). Apple's vocabulary in AGXCompilerCore has
    137 names; the ones no module calls are not translated and not tested, and
    this prints that difference rather than hiding it.
  - for each declaration a fragment shader is generated that calls it on
    constant arguments and returns the result as its render target, llvm-as
    assembles it, and `air2nir --eval` folds it. Nothing is read from the
    corpus but the list of names.
  - the reference is this file's Python, which for every exactly-defined
    operation is the IEEE result rounded once to the shader's precision.

What a disagreement means, in order of what it is most likely to be: the
lowering in air_intrinsics.c is the wrong NIR opcode (fast_round); the
reference here is the wrong Metal semantics; NIR's constant folder differs
from libm. The third has never happened yet.

What this does NOT prove, and no amount of it will:

  - The reference for a transcendental is this machine's libm, not Apple's and
    not the Adreno's. Metal's `fast_` prefix *permits* a lower-precision
    implementation, so the printed difference for fast_sin is a measurement,
    not a verdict. Those are reported separately and are never counted as
    exact. The only thing checked there is that the lowering picked an
    operation of the right shape.
  - NIR's constant folder is not the Adreno. A difference between what
    nir_opt_constant_folding computes for nir_ffloor and what the hardware's
    floor instruction computes would be invisible here. Checking that needs
    the phone, and for these operations it has not been done.
  - Denormals. Apple compiles with air.compile.denorms_disable, so the
    hardware may flush where this check does not. Inputs are kept normal.

usage: intrincheck.py [-v] [--decls FILE] [OP ...]           fold it, exhaustively
       intrincheck.py --run [--few] [--cases N] [OP ...]     run it through a real driver
       intrincheck.py --emit DIR [--few] [--cases N]         write it out to carry somewhere
       intrincheck.py --check DIR RUN.LOG                    judge what the runner printed
"""

import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir")
)

# ---------------------------------------------------------------------------
# precision


def f32(x):
    """x rounded once to float32, the way a shader's float holds it."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def f16(x):
    try:
        return struct.unpack("<e", struct.pack("<e", float(x)))[0]
    except (OverflowError, struct.error):
        return math.copysign(float("inf"), x)


def f64(x):
    """x as the shader's `double` holds it, which is what a Python float is."""
    return float(x)


def to_f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def to_f64_bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def same_float(g, exp, rty):
    """Did the shader return the expected float, bit for bit at its own width?

    Bit patterns and not `==`, because that is what separates 0 from -0.
    AT ITS OWN WIDTH, because `to_f32_bits(f32(...))` on a `double` compares
    two numbers after rounding both to a type neither of them is -- which
    would have made air.floor.f64 agree with half its mantissa thrown away.
    """
    if math.isnan(g) and math.isnan(exp):
        return True
    if rty == "double":
        return to_f64_bits(g) == to_f64_bits(exp)
    return to_f32_bits(f32(g)) == to_f32_bits(f32(exp))


def smallest_normal(rty):
    """The threshold below which a result is not compared, by type.

    A denormal result is not compared at all (Metal compiles with
    air.compile.denorms_disable, so flushing one to zero is permitted), and
    f32's 1.17549435e-38 applied to a `double` would call 1e-300 a denormal
    and drop a case that is perfectly normal where it lives.

    A `half` deliberately keeps f32's threshold rather than its own 2^-14.
    Its own would be the defensible reading of denorms_disable, but it would
    also stop comparing every half result between 2^-14 and 2^-126 -- cases
    that are compared today and agree -- and quietly narrowing a check that
    passes is the one move this file exists to prevent. Whether a half
    denormal may be flushed is a separate question and is open.
    """
    return 2.2250738585072014e-308 if rty == "double" else 1.17549435e-38


def wrap(bits, width, signed):
    bits &= (1 << width) - 1
    if signed and bits >> (width - 1):
        bits -= 1 << width
    return bits


# ---------------------------------------------------------------------------
# the inputs
#
# Every value here is one that has broken something, in this port or in
# somebody else's: the half-way cases that separate round from rint, the
# largest float below a half-way case (0x1.fffffep-2, which the first fix for
# fast_round got wrong), the powers of two where exponent arithmetic goes
# wrong, zero of both signs, and the first float above 2^24 where consecutive
# integers stop being representable.

FLOATS = [
    0.0,
    -0.0,
    0.5,
    -0.5,
    struct.unpack("<f", struct.pack("<I", 0x3EFFFFFF))[0],  # 0x1.fffffep-2
    1.5,
    -1.5,
    2.5,
    -2.5,
    1.0,
    -1.0,
    3.7,
    -3.7,
    0.1,
    -0.1,
    2.0,
    16777216.0,  # 2^24
    16777217.0,  # not representable: becomes 2^24
    1e-8,
    1e8,
    123.456,
    -123.456,
]

# the second and third arguments want their own values, not the same ones, or
# min(a, b) is only ever tested with a == b
# -0.0 belongs here as well as in FLOATS: all four of (+-0, +-0) are separate
# cases for atan2, and three of them were wrong until this pool had them
FLOATS2 = [1.0, -1.0, 0.0, -0.0, 0.25, -0.25, 2.0, 3.0, -3.0, 0.5, 100.0, -100.0, 1e-8]

# inputs where the answer is defined but unusual. Reported separately: a
# disagreement here is worth reading, not necessarily worth fixing, because
# fast-math shaders are allowed to give up on them.
EDGE_FLOATS = [float("inf"), float("-inf"), float("nan")]

INTS = [0, 1, -1, 2, 7, -7, 127, -128, 255, 32767, -32768, 65535, 1000000, -1000000,
        2147483647, -2147483648]

# Added to INTS for an i64 operand only, because WITHOUT ONE OF THESE THE WHOLE
# 64-BIT FAMILY IS DECIDED BY VALUES WHOSE TOP WORD IS 0 OR ALL ONES. Every
# value of INTS fits in 32 bits, so a lowering that computed a 64-bit min on
# the low word alone and sign-extended it would answer correctly on all 192
# cases of air.min.u.i64: the operand whose low word is smaller is also the
# operand that is smaller.
#
# What separates the two is a value whose low word is SMALL while the value
# itself is LARGE. 2^32 is the cleanest -- low word 0, so a low-word
# comparison picks it every time -- and umin(2^32, 1) is 1 while the truncating
# answer is 2^32 or 0, neither of which is 1. The pool's first argument is
# taken whole and the others are not (inputs_for), which is enough: one operand
# of this shape in a comparison decides it.
#
# These go at the END, where they cannot move what `--cases N` keeps.
I64S = [1 << 32,              # low word 0, high word 1
        (1 << 32) - 1,        # the largest value with a zero high word
        (1 << 32) + 1,        # low word 1, high word 1: 2^32 + 1 > 2^32 - 1
        (1 << 40) + 3,
        (1 << 63) - 1,        # the largest signed i64
        -(1 << 63),           # the smallest, and 0x8000000000000000 unsigned
        -(1 << 32)]           # low word 0, high word all ones

# ---------------------------------------------------------------------------
# the reference
#
# One entry per base operation: how many arguments, what it computes, and
# whether the result is exactly defined. `exact=False` means the reference is
# this machine's libm and the shader is allowed to differ -- the number is
# printed and nothing is concluded from it.


class Undecided(ValueError):
    """A case this reference declines to answer, and the reason why.

    Not the same thing as a case that is out of range or out of domain, which
    is simply skipped: this is a case where the sources disagree about what
    the answer should be, so asserting any of them would be inventing a
    verdict. The reason travels with the exception, is counted per operation,
    and is printed -- a declined case must never look like a case that passed.

    It subclasses ValueError on purpose: emit_suite, airdefs.py and the
    run-stage generator already drop a case whose reference raises ValueError,
    so they keep working unchanged and only the folding check, which can
    report it, tells them apart.
    """


def _signed_int_result(f):
    """math.floor and friends return a Python int, which cannot hold -0. IEEE
    says floor(-0) is -0, and the shader gets that right, so the reference has
    to as well -- the first thing this check found was its own reference."""
    def g(x):
        if math.isnan(x) or math.isinf(x):
            return x
        return math.copysign(float(f(x)), x)
    return g


# The precision the operation under test computes at, set by reference()
# before it calls one of these. An operation on halves rounds every
# intermediate to half: mix(3.7h, 1h, 0.25h) is 3.0234375 that way and
# 3.02441406 with float32 intermediates, and the shader is right.
ROUND = f32


def _mix(a, b, t):
    return ROUND(a + ROUND(ROUND(b - a) * t))


def _fract(x):
    if math.isnan(x):
        return x
    if math.isinf(x):
        # Three sources, three answers, and no oracle on this machine -- so
        # this is declined rather than decided:
        #
        #   Metal says fract returns a value in [0, 1), which an infinity is
        #     not: the argument is outside the function as specified.
        #   OpenCL, whose math library MSL's is, has an edge-case table that
        #     gives fract(+-inf) = +-0.
        #   NIR's ffract is "src0 - floorf(src0)" (nir_opcodes.py), which is
        #     inf - inf, so the frontend folds a NaN -- and that is what the
        #     Adreno's instruction would have to be asked about.
        #
        # This reference used to answer `inf`, which is the one answer none of
        # the three supports: the guard above returned the argument for an
        # infinity as well as for a NaN, bypassing this function's own clamp
        # to below 1 four lines down. air.fract.f16 was folded for the first
        # time on 21 October and reported three cases wrong on the strength of
        # it.
        #
        # It is only ever asked because the pool's 2^24, 2^24+1 and 1e8
        # overflow f16, which makes air.fract.f16 the one declaration here
        # given an infinity at all -- and the device's own metal_math routes
        # `half fract(half)` through __METAL_MAYBE_FAST_MATH__, which
        # metal_common defines as __METAL_FAST_MATH__ whenever __FAST_MATH__
        # is set. The corpus says it is: its modules carry `fmul fast` and
        # air.compile.denorms_disable. Under fast math an implementation is
        # permitted to assume there are no infinities at all. docs/air.md 4.
        raise Undecided("fract of an infinity: Metal's result is in [0, 1), "
                        "OpenCL's edge table says +-0, NIR's ffract computes "
                        "inf - inf = nan, and nothing here can decide between "
                        "them")
    # the sign of floor's zero decides the sign of this zero:
    # -0 - (-0) is +0, but -0 - 0 is -0, and Python's math.floor returns an
    # int, which has no -0 to carry
    r = ROUND(x - math.copysign(float(math.floor(x)), x))
    # Metal: "returns a value in [0, 1)", so a result that rounds up to 1 is
    # clamped to the largest value below it in this precision
    return min(r, ROUND(1.0 - ROUND(2.0 ** -12)))


def _sign(x):
    if math.isnan(x):
        return 0.0
    if x > 0:
        return 1.0
    if x < 0:
        return -1.0
    return x  # keeps the sign of zero


def _roundf(x):
    """C roundf: half away from zero, which is Metal's round."""
    if math.isnan(x) or math.isinf(x) or x == 0.0:
        return x
    a = math.floor(abs(x) + 0.5)
    # floor(|x| + 0.5) is wrong where |x| + 0.5 rounds up in float; do it the
    # way the shader does, and the way roundf is specified
    t = math.trunc(abs(x))
    a = t + 1 if abs(x) - t >= 0.5 else t
    return math.copysign(a, x)


def _fmin(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    if a == b == 0.0:  # fmin(+0, -0) may be either; Metal does not say
        return b if math.copysign(1, b) < 0 else a
    return min(a, b)


def _fmax(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    if a == b == 0.0:
        return b if math.copysign(1, b) > 0 else a
    return max(a, b)


def _clamp_f(x, lo, hi):
    return _fmin(_fmax(x, lo), hi)


def _powr(x, y):
    # powr is pow for x >= 0 only, undefined elsewhere
    if x < 0:
        return float("nan")
    return math.pow(x, y)


# Python raises where C returns a value. Metal's math functions are OpenCL's,
# which keep the IEEE results at the edge of the domain, so raising here would
# silently drop the cases that matter most -- it had already dropped every case
# of air.fast_sqrt.v4f32, whose rotated components always include a negative.
def _sqrt(x):
    if math.isnan(x):
        return x
    return float("nan") if x < 0 else math.sqrt(x)   # sqrt(-0) is -0


def _log(f, zero):
    def g(x):
        if math.isnan(x):
            return x
        if x == 0.0:
            return zero
        return float("nan") if x < 0 else f(x)
    return g


def _rsqrt(x):
    if math.isnan(x):
        return x
    if x == 0.0:
        return math.copysign(float("inf"), x)
    return float("nan") if x < 0 else 1.0 / math.sqrt(x)


def _fmod(x, y):
    if y == 0.0 or math.isnan(x) or math.isnan(y) or math.isinf(x):
        return float("nan")
    return math.fmod(x, y)


def _pow(x, y):
    # C: pow(+-0, y) for a negative odd integer y is +-inf, keeping the zero's
    # sign; for any other negative y it is +inf. Python raises for both, and
    # the fallback that guessed +inf made the shader's correct -inf for
    # pow(-0, -1) look like an infinite-ulp disagreement.
    if x == 0.0 and y < 0:
        odd = y == int(y) and int(y) % 2 != 0
        return math.copysign(float("inf"), x) if odd else float("inf")
    try:
        return math.pow(x, y)
    except ValueError:
        return float("nan")        # x < 0 with a fractional y
    except OverflowError:
        return math.copysign(float("inf"), x if x < 0 and y == int(y)
                             and int(y) % 2 != 0 else 1.0)


def _inverse_trig(f):
    """asin and acos, which are NaN outside [-1, 1] rather than absent.

    Python raises where C returns a value, and a raise here would drop the
    cases that decide the most: the shader's lowering goes through a square
    root of (1 - x)(1 + x), so an argument outside the domain is exactly the
    one that must come back NaN and not a plausible angle. The same reason
    _sqrt above returns NaN rather than raising.
    """
    def g(x):
        if math.isnan(x):
            return x
        if x < -1.0 or x > 1.0:
            return float("nan")
        return f(x)
    return g


def _hyperbolic(f, odd):
    """sinh and cosh, whose answer for a large argument is an infinity.

    math.sinh raises OverflowError where the shader -- and C, and OpenCL,
    whose library Metal's is -- gives +-inf. Skipping those would leave the
    pool's 1e8 and 2^24 untested, which are the only arguments that reach the
    saturation at all. sinh is odd and keeps the argument's sign; cosh is even
    and overflows to +inf from either side.
    """
    def g(x):
        try:
            return f(x)
        except OverflowError:
            return math.copysign(float("inf"), x) if odd else float("inf")
    return g


def _ldexp(x, n):
    try:
        return math.ldexp(x, n)
    except OverflowError:
        return math.copysign(float("inf"), x)


OPS = {
    # name:        (nargs, fn, exact)
    "floor":       (1, _signed_int_result(math.floor), True),
    "ceil":        (1, _signed_int_result(math.ceil), True),
    "trunc":       (1, _signed_int_result(math.trunc), True),
    "rint":        (1, _signed_int_result(round), True),
    "round":       (1, _roundf, True),
    "fabs":        (1, abs, True),
    "fract":       (1, _fract, True),
    "sign":        (1, _sign, True),
    "saturate":    (1, lambda x: _clamp_f(x, 0.0, 1.0), True),
    "fmin":        (2, _fmin, True),
    "fmax":        (2, _fmax, True),
    "clamp":       (3, _clamp_f, True),
    "mix":         (3, _mix, True),
    "fmod":        (2, _fmod, True),
    "mod":         (2, _fmod, True),
    "divide":      (2, lambda a, b: a / b if b else math.copysign(float("inf"), a) if a else float("nan"), True),
    "sqrt":        (1, _sqrt, True),      # IEEE: exactly rounded
    "fma":         (3, lambda a, b, c: math.fma(a, b, c), True),
    # llvm.fmuladd, which this frontend translates FUSED -- one rounding, the
    # same as fma. Counted as exactly defined although the intrinsic itself is
    # not: LLVM's reference leaves it unspecified whether the product is
    # rounded first, so a disagreement here does not mean the frontend is
    # wrong, it means the frontend changed its mind about which of the two
    # legal answers it gives. That is worth catching either way, and the
    # softpipe run stage sets it aside beside air.fma for the reason written
    # there (TGSI's MAD is allowed to round the product, and does).
    "fmuladd":     (3, lambda a, b, c: math.fma(a, b, c), True),
    "ldexp":       (2, _ldexp, True),
    # approximate: the reference is libm, the shader is allowed to differ
    "sin":         (1, math.sin, False),
    "cos":         (1, math.cos, False),
    "tan":         (1, math.tan, False),
    "atan2":       (2, math.atan2, False),
    # The inverse trigonometric and hyperbolic ones. Approximate for the same
    # reason as sin and cos -- the reference is this machine's libm, and
    # Metal's fast_ prefix licenses a deviation -- but the number is still
    # worth having: the lowering of acos and asin goes through atan2, whose
    # own polynomial is Mesa's, so a large ulp count here is a measurement of
    # that polynomial and a wrong formula would show as a quadrant, a sign or
    # a NaN rather than as a few ulp.
    "acos":        (1, _inverse_trig(math.acos), False),
    "asin":        (1, _inverse_trig(math.asin), False),
    "atan":        (1, math.atan, False),
    "sinh":        (1, _hyperbolic(math.sinh, True), False),
    "cosh":        (1, _hyperbolic(math.cosh, False), False),
    "tanh":        (1, math.tanh, False),
    "exp":         (1, math.exp, False),
    "exp2":        (1, lambda x: math.pow(2.0, x), False),
    "exp10":       (1, lambda x: math.pow(10.0, x), False),
    "log":         (1, _log(math.log, float("-inf")), False),
    "log2":        (1, _log(math.log2, float("-inf")), False),
    "log10":       (1, _log(math.log10, float("-inf")), False),
    "pow":         (2, _pow, False),
    "powr":        (2, _powr, False),
    "rsqrt":       (1, _rsqrt, False),
}

# integer operations, by (base name, signed)
IOPS = {
    ("min", True):  lambda w: (2, min),
    ("min", False): lambda w: (2, min),
    ("max", True):  lambda w: (2, max),
    ("max", False): lambda w: (2, max),
    ("clamp", True): lambda w: (3, lambda x, lo, hi: min(max(x, lo), hi)),
    ("clamp", False): lambda w: (3, lambda x, lo, hi: min(max(x, lo), hi)),
}


# The one set-aside this check applies to a DISAGREEMENT rather than to a case
# nobody can ask. It is in the same family as the fract-of-an-infinity case the
# reference declines and the narrow types softpipe cannot run: a difference
# whose cause is written down, whose extent is a stated condition, and which is
# counted and named rather than either hidden or left as an unknown failure.
#
# What is being set aside: air_fmod computes fma(-y, trunc(x/y), x) with the
# quotient corrected by one either way and the sign of a zero taken from the
# dividend. That is bit-exact for every pair whose quotient is exactly
# representable and CANNOT be for any other -- measured, not argued: over
# 195 906 random pairs of normal floats every disagreement with this machine's
# libm has |x/y| >= 2^24, and over 376 438 whose quotient is representable
# there is none. The exact answer above that needs an iterated reduction of
# twelve passes (each removes only 22 bits of an exponent difference that
# reaches 253), and its exactness ON THE ADRENO cannot be established from this
# machine at all, because ir3 lowers fdiv to fmul(a, frcp(b)) and frcp is
# approximate. Shipping it would mean shipping a routine whose only claim
# cannot be checked here, so it is not shipped and this is the honest verdict
# instead.
#
# WHAT MAKES IT NARROW RATHER THAN AN EXCUSE: it is decided case by case, not
# per declaration. A disagreement that does not satisfy the condition is a
# failure and fails the stage, which is the whole point -- if the Adreno's
# frcp costs more than air_fmod's two corrections can absorb, the cases that
# show it have |x/y| < 2^24 and will come through as WRONG.
FMOD_ASIDE = (
    "fmod with |x/y| >= 2^24 (2^11 for a half): trunc(x/y) is not exactly "
    "representable there, so no correction of a bounded size can recover it, "
    "and the exact answer needs a twelve-pass reduction whose exactness on the "
    "Adreno cannot be established on this machine because ir3 lowers fdiv to "
    "fmul(a, frcp(b)). Decided case by case -- a disagreement that does not "
    "satisfy it fails. air_fmod in air_intrinsics.c, docs/air.md section 4")


def fmod_aside(args, c, rty):
    """Is THIS differing case of an fmod one the lowering never claimed?

    True only for a finite pair with a non-zero divisor whose quotient is
    outside the type's exactly representable integers. Everything else --
    including a disagreement at a small ratio, which is what a wrong lowering
    would look like -- is a failure.
    """
    if len(args) < 2:
        return False
    try:
        rnd = f16 if rty == "half" else f64 if rty == "double" else f32
        x = rnd(args[0][c] if len(args[0]) > c else args[0][0])
        y = rnd(args[1][c] if len(args[1]) > c else args[1][0])
    except (TypeError, IndexError, OverflowError, ValueError):
        return False
    if not (math.isfinite(x) and math.isfinite(y)) or y == 0.0:
        return False
    try:
        return abs(x / y) >= 2.0 ** (11 if rty == "half" else 24)
    except (ZeroDivisionError, OverflowError):
        return False


def ref_elementwise(op, args, argtys, signed):
    """One component of an elementwise operation.

    The arguments must be rounded to the shader's precision first. mix(3.7, 1,
    1) is exactly 1 in float32 and 0.99999994 in double, and this check
    reported the shader wrong for a while because the reference was given
    Python's 3.7 and the shader f32(3.7). Everything insensitive to the low
    bits of its input -- floor, min, clamp -- hid the mistake.
    """
    bty, _ = parse_type(argtys[0])
    if bty.startswith("i"):
        # integer min/max/clamp: no rounding anywhere, and the result is
        # whatever the declared width holds
        w = int(bty[1:])
        fn = {"min": min, "max": max,
              "clamp": lambda x, lo, hi: min(max(x, lo), hi)}[op]
        # air.min.u is an unsigned minimum: umin(0, 0xffff) is 0, not -1, and
        # taking the declaration's `u` for granted as signed made the shader
        # look wrong on 175 of 256 cases
        return wrap(fn(*[wrap(v, w, bool(signed)) for v in args]), w, bool(signed))

    global ROUND
    # AT THE RESULT'S OWN PRECISION. `f32` for a double would round the answer
    # to 24 bits of mantissa and then compare it with a shader that kept 53,
    # which is how air.floor.f64 would have "agreed" about something else.
    ROUND = f16 if bty == "half" else f64 if bty == "double" else f32
    nargs, fn, exact = OPS[op]
    rounded = [ROUND(v) if parse_type(ty)[0] in ("half", "float") else v
               for ty, v in zip(argtys, args)]
    return ROUND(fn(*rounded))


def ref_convert(name, retty, srcty, v):
    """air.convert.<dk>.<dt>.<sk>.<st>(v), one component.

    The two kind letters are not decoration. `s`/`u` on the source say how its
    integer is read, which decides sign- against zero-extension; `s`/`u` on the
    destination say how the result is read, which decides whether a float is
    converted signed or unsigned and what a narrowing wraps to. The corpus has
    air.convert.u.v2i16.s.v2i32 -- a signed 32-bit value narrowed to an
    unsigned 16-bit one -- where getting either letter the wrong way round
    still produces a plausible number.
    """
    parts = name.split(".")
    dk, sk = parts[2], parts[4]
    dbase, _ = parse_type(retty)
    sbase, _ = parse_type(srcty)

    if dbase == "i1":
        # A BOOL DESTINATION IS NOT A CONVERSION, and this reference used to
        # treat it as one: truncate toward zero, then keep one bit. That makes
        # bool(0.1) false, and bool(2.0) false again, which is not C's rule,
        # not C++11's, and not the one MSL inherits from either -- the rule is
        # `x != 0`.
        #
        # Apple's own compiler says the same thing by what it does with the
        # result. Every call to air.convert.u.i1.f.f32 in the device image --
        # two in QuartzCore's default.metallib, two in AppleCVAPhoto's
        # cvadepth-iOS.metallib -- feeds a `br i1` directly, e.g.
        #
        #     %590 = fpext half %336 to float
        #     %591 = tail call i1 @air.convert.u.i1.f.f32(float %590)
        #     br i1 %591, label %592, label %593
        #
        # which is an `if (x)` on a floating-point value. Under a truncating
        # reading that branch would also be a branch on poison, because LLVM's
        # own rule for `fptoui float to i1` is poison whenever the truncated
        # value does not fit in the destination, which is every |x| >= 2. No
        # compiler emits that for a condition.
        #
        # air_intrinsics.c lowers this as a comparison and says why
        # ("A BOOL DESTINATION IS NOT A CONVERSION", nir_fneu_imm): the
        # reference is what was wrong. Nothing had evaluated it before. The
        # declaration only entered this check's list late on 21 October --
        # 688f5de's own commit message records check.sh still reading
        # "intrincheck 71/0" with that commit's f2u1 fix already in -- so an
        # expectation nobody had ever folded had no reason to be right.
        if sbase in ("half", "float"):
            x = f16(v) if sbase == "half" else f32(v)
            truth = x != 0.0          # a NaN is true, as `fcmp une` makes it
        else:
            truth = wrap(v, int(sbase[1:]), sk == "s") != 0
        return wrap(1 if truth else 0, 1, dk == "s")

    if sbase in ("half", "float"):
        x = f16(v) if sbase == "half" else f32(v)
        if dbase in ("half", "float"):
            # FLOAT -> FLOAT, which this branch used to fall straight through
            # into the float -> int code below and crash in it: `int("alf")`
            # for a `half` destination, `int("loat")` for a `float` one. Every
            # case of the six float/half conversions the corpus declares --
            # air.convert.f.v{2,3,4}f16.f.v{2,3,4}f32 and the three the other
            # way -- therefore raised ValueError, was dropped by the bare
            # `continue` that used to catch it, and left the declaration
            # reported as "agrees on every case" over ZERO cases. 123 cases
            # and 6 declarations. It took counting the dropped cases to see
            # it, which is the whole argument for counting them.
            #
            # The answer is one rounding to the destination's precision:
            # exact widening one way, round-to-nearest-even the other. An
            # infinity and a NaN pass through, and f16() turns an overflow
            # into the infinity f32 -> f16 must produce.
            return f16(x) if dbase == "half" else f32(x)
        # float -> int: Metal truncates toward zero. Out of the destination's
        # range it is undefined, in C and in the MSL specification both, so
        # those are not cases.
        if math.isnan(x) or math.isinf(x):
            raise ValueError("not defined")
        w = int(dbase[1:])
        t = math.trunc(x)
        lo, hi = ((-(1 << (w - 1)), (1 << (w - 1)) - 1) if dk == "s"
                  else (0, (1 << w) - 1))
        if t < lo or t > hi:
            raise ValueError("outside the destination's range")
        return wrap(t, w, dk == "s")

    w = int(sbase[1:])
    x = wrap(v, w, sk == "s")
    if dbase in ("half", "float"):
        # int -> float, rounded once to the destination's precision
        return f16(x) if dbase == "half" else f32(x)
    return wrap(x, int(dbase[1:]), dk == "s")


def reference(family, op, name, args, argtys, retty, signed):
    """The reference result for one case, as a list of result components.

    `args` is one list per argument, holding that argument's components.
    """
    rty, rn = parse_type(retty)

    if family == "dot":
        # The inputs are small dyadic rationals, so every product and every
        # partial sum is exact and the answer does not depend on the order the
        # accumulation is taken in -- checked, not assumed: over all 300 cases
        # of the DOT_FLOATS pool the left fold, the right fold, the pairwise
        # order NIR's own fdot3/fdot4 constant expressions use ((p2+p1)+p0 and
        # (p3+p2)+(p1+p0)) and the once-rounded exact sum are one and the same
        # number, in f16 as in f32. What this pins is which components are
        # paired with which -- a lowering that dropped a component or paired
        # them crosswise gives a different number -- and not the rounding of a
        # general dot product, which it cannot.
        #
        # THE PRECISION IS THE RESULT'S, not f32. This branch rounded to f32
        # whatever the declaration returned, and for a half dot product that
        # is an answer the type cannot hold: (-0.25, 8, -16) against itself is
        # exactly 320.0625, and f16's step at 320 is 0.25, so 320 is the only
        # thing a `half` can be. air.dot.v3f16 and air.dot.v4f16 were folded
        # for the first time on 21 October and were duly reported wrong on the
        # three of their 200 cases whose exact sum wants a 13-bit significand
        # from a type that has 11 (air.dot.v2f16 never reaches one). Three say
        # the same 320: f16 arithmetic itself, llc on this processor through
        # the half `fmul`/`fadd` bodies of airdefs.py, and air2nir's folder.
        # The reference was the only one of the four that said otherwise.
        rnd = f16 if rty == "half" else f32
        n = parse_type(argtys[0])[1]
        acc = 0.0
        for k in range(n):
            # +0.0 + p0, then + p1 ...: the order airdefs.py's IR emits, so
            # the two references stay comparable instruction for instruction
            acc = rnd(acc + rnd(rnd(args[0][k]) * rnd(args[1][k])))
        return [acc]

    if family in ("all", "any"):
        n = parse_type(argtys[0])[1]
        v = args[0][:n]
        return [1 if (all(v) if family == "all" else any(v)) else 0]

    if family == "convert":
        return [ref_convert(name, retty, argtys[0], v) for v in args[0][:rn]]

    out = []
    for k in range(rn):
        # a scalar argument of a vector operation would be broadcast; none of
        # the corpus's declarations mix the two, but take component 0 if so
        per = [a[k] if len(a) > k else a[0] for a in args]
        out.append(ref_elementwise(op, per, argtys, signed))
    return out


# ---------------------------------------------------------------------------
# the declarations
#
# air.<op>.<type>                       air.fast_round.f32
# air.<op>.<s|u>.<type>                 air.clamp.s.v2i16
# air.convert.<f|s|u>.<dst>.<f|s|u>.<src>
# air.dot.<type>                        returns a scalar from two vectors

# air.* and the handful of llvm.* this frontend translates. llvm.fmuladd and
# llvm.fabs belong in this check for the same reason as the arithmetic they sit
# among: they are operations with a reference, 124 of Resident Evil 4's modules
# stop at one of them when it is missing, and nothing else here would notice if
# the lowering changed.
DECL = re.compile(r"^declare\s+(.*?)\s+@((?:air|llvm)\.[A-Za-z0-9_.]+)\((.*)\)")


def parse_type(s):
    """'<4 x float>' -> ('float', 4); 'i32' -> ('i32', 1)"""
    s = s.strip()
    m = re.match(r"<(\d+) x (.+)>$", s)
    if m:
        return m.group(2).strip(), int(m.group(1))
    return s, 1


# ---------------------------------------------------------------------------
# how a result is carried back through a four-lane render target
#
# A render target lane is 32 bits wide -- air_run reads back through
# R32G32B32A32, and --eval prints the four components of an int4 or float4 --
# so a result component that is wider than 32 bits has to be carried in two
# lanes and put back together here.
#
# It was not. `trunc i64 %c to i32` was emitted into the generated shader and
# the low half was then compared against a 64-bit reference, so EVERY i64
# result whose top word was not zero was reported as the frontend's mistake:
# 16 declarations and 2 164 cases, with `air.min.u.i64(-1, -1)` reading "got
# 4294967295 want 18446744073709551615". The frontend was right all along --
# air2nir folds that call to 0xffffffffffffffff and softpipe executes it to
# the same bits (docs/air.md section 4) -- and the truncation was the
# harness's own. A measurement instrument that quietly drops the top half of
# what it measures is worse than no instrument: this one accused the compiler.


def lanes_per_comp(bty):
    """32-bit render-target lanes one component of type `bty` occupies.

    A `double` is here for the same reason an `i64` is, and it was NOT: the
    generated shader put a double straight into a <4 x float> and llvm-as said
    so -- "'%c0' defined with type 'double' but expected 'float'" -- which was
    reported as `2 could not be built or did not fold`, a REFUSED row whose
    reason was a formatting mistake in this harness and nothing about the
    frontend. The frontend is fine: air.floor.f64(0x1.ccccccccccccdp+0) folds
    to 0x3FF0000000000000, exactly 1.0. So a double is carried as its two
    32-bit halves through an int4 target and put back together by unlane,
    exactly as an i64 is, and compared at 64 bits by same_float.
    """
    return 2 if bty in ("i64", "double") else 1


def comp_batches(retty):
    """[(components, cases per shader)]: how to split a result over shaders.

    A four-lane target holds `4 // lanes_per_comp` components, so a v4i64
    result does not fit in one shader at all and its components are split over
    two. Returning the split rather than assuming one shader per case is what
    keeps a component from being silently dropped -- the packing below cuts
    the lane list to four, so a v4i64 would have had components 2 and 3
    quietly never measured.
    """
    bty, rn = parse_type(retty)
    lanes = lanes_per_comp(bty)
    per_group = max(1, 4 // lanes)
    out = []
    for k in range(0, rn, per_group):
        comps = list(range(k, min(k + per_group, rn)))
        out.append((comps, max(1, 4 // (lanes * len(comps)))))
    return out


def unlane(flat, ncases, comps, bty, signed):
    """The four lanes of one shader's target, back as [[component]] per case.

    `flat` holds the lanes as they were printed: a float for a float target,
    an unsigned 32-bit word for an integer one.
    """
    lanes = lanes_per_comp(bty)
    out = []
    for i in range(ncases):
        row = []
        for p in range(len(comps)):
            base = (i * len(comps) + p) * lanes
            if base + lanes > len(flat):
                return None          # a short row is the caller's to report
            if lanes == 2:
                bits = ((flat[base] & 0xFFFFFFFF)
                        | ((flat[base + 1] & 0xFFFFFFFF) << 32))
                if bty == "double":
                    row.append(struct.unpack("<d", struct.pack("<Q", bits))[0])
                else:
                    row.append(wrap(bits, 64, bool(signed)))
            elif isinstance(flat[base], float):
                row.append(flat[base])
            else:
                row.append(wrap(flat[base], 32, bool(signed)))
        out.append(row)
    return out


def not_run_at_all(why):
    """Does this reason mean the BINARY could not be run, rather than that the
    library could not be read?

    The distinction decides whether a run is contaminated or merely limited.
    `not LLVM bitcode` is a fact about the library and will say the same thing
    tomorrow; EACCES, exit 126 and exit 127 are a fact about the moment -- this
    tree is shared and air2nir is relinked under the check -- so a run that saw
    one measured less than it thinks and must not be reported as a pass.
    """
    return ("Permission denied" in why or "Text file busy" in why
            or "No such file" in why
            or why.startswith("exit 126") or why.startswith("exit 127"))


def collect_declarations(libs):
    """(the declarations, the libraries that could not be read).

    THE SECOND HALF OF THAT PAIR IS THE POINT. This used to be `except
    OSError: continue` over a subprocess whose exit code was never looked at,
    so a library air2nir could not be executed for -- and this tree is shared,
    so the binary is relinked under the check several times a night, which is
    EACCES or exit 126 -- dropped out of the vocabulary without a word. Fewer
    declarations is fewer operations tested and a HIGHER pass rate, which is
    the one direction a broken meter must never move in: older logs read
    "134 agree, 0 differ" where the vocabulary is 553 declarations wide.
    A library that was not read is named and counted by the caller.
    """
    out = set()
    unread = []
    for lib in libs:
        try:
            q = subprocess.run([AIR2NIR, "--ll", lib], capture_output=True)
        except OSError as e:
            unread.append((lib, str(e)))
            continue
        if q.returncode:
            err = q.stderr.decode("utf-8", "replace").strip().splitlines()
            unread.append((lib, "exit %d%s" % (q.returncode,
                                               ": " + err[-1] if err else "")))
            continue
        for line in q.stdout.decode("utf-8", "replace").splitlines():
            if line.startswith("declare ") and ("@air." in line or
                    re.search(r"@llvm\.(fmuladd|fabs)\.", line)):
                line = re.sub(r"\s+local_unnamed_addr.*$", "", line)
                line = re.sub(r"\s+#\d+$", "", line)
                out.add(line.strip())
    return sorted(out), unread


# ---------------------------------------------------------------------------
# the generated shader

SKELETON = """source_filename = "ic"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x %(outty)s> }> @ic() local_unnamed_addr {
%(body)s
  %%r = insertvalue <{ <4 x %(outty)s> }> undef, <4 x %(outty)s> %%v3, 0
  ret <{ <4 x %(outty)s> }> %%r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ic, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"%(outname)s", !"air.arg_name", !"out"}
!6 = !{}
"""

# The same shader with its arguments read from a constant buffer instead of
# written in as immediates. That is the only form worth running on a GPU: ir3
# folds, so air.fast_round(0.5) written as a constant is answered by the
# compiler and the hardware never sees the instruction -- the test would pass
# with the GPU switched off. A value loaded from a buffer cannot be folded, and
# air_run puts bytes we choose there (AIR_RUN_BUFFER).
BUF_SKELETON = """source_filename = "ic"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x %(outty)s> }> @ic(ptr addrspace(2) %%in) local_unnamed_addr {
%(body)s
  %%r = insertvalue <{ <4 x %(outty)s> }> undef, <4 x %(outty)s> %%v3, 0
  ret <{ <4 x %(outty)s> }> %%r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ic, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"%(outname)s", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4096, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"in"}
"""

# One case's arguments occupy MAXARG * SLOTS_PER_ARG four-byte slots of that
# buffer, so a component's address is arithmetic on its indices and nothing has
# to be packed tightly.
#
# SLOTS_PER_ARG is eight and not four because an i64 component takes two slots:
# with four, a `load i64` of component 0 of a v4i64 argument read its own low
# word and COMPONENT 1's low word as its high word, so every 64-bit vector
# operand in the run stage was fed a number nobody chose. The slots are also
# the reason a component's address stays 8-byte aligned: 24 slots a case, 8 a
# argument, 2 a component, all even.
MAXARG = 3
SLOTS_PER_ARG = 8
SLOTS_PER_CASE = MAXARG * SLOTS_PER_ARG


def slot_bytes(bty, v):
    """The slots holding `v` as type `bty`, little-endian: 4 bytes, or 8 for i64.

    A narrow type is written into the low bytes of its slot, which is where a
    little-endian load of that width at a four-byte-aligned address reads it.
    An i64 gets both of its slots -- it used to get `struct.pack("<i", ...)` of
    its low word alone, and the load then took the next slot as its high word.
    """
    if bty == "float":
        return struct.pack("<f", f32(v))
    if bty == "half":
        h = f16(v)
        b = (struct.pack("<H", 0x7C00 if h > 0 else 0xFC00) if math.isinf(h)
             else struct.pack("<e", h))
        return b + b"\0\0"
    if bty == "double":
        return struct.pack("<d", v)
    w = int(bty[1:])
    if w == 1:
        return struct.pack("<B", 1 if v else 0) + b"\0\0\0"
    if w == 64:
        return struct.pack("<q", wrap(v, 64, True))
    return struct.pack("<i", wrap(v, 32, True) if w >= 32
                       else wrap(v, w, True) & ((1 << w) - 1))


def split64(lines, i, k, s_):
    """Append the IR that splits one i64 into its low and high words.

    Two lanes of the render target, low first, which is what unlane() puts
    back together. `lshr` and not `ashr`: the halves are bit patterns here and
    the sign is applied once, to the whole 64-bit value, by wrap() in unlane.
    """
    lo = "%%lo%d_%d" % (i, k)
    sh = "%%sh%d_%d" % (i, k)
    hi = "%%hi%d_%d" % (i, k)
    lines.append("  %s = trunc i64 %s to i32" % (lo, s_))
    lines.append("  %s = lshr i64 %s, 32" % (sh, s_))
    lines.append("  %s = trunc i64 %s to i32" % (hi, sh))
    return [lo, hi]


def emit_buffered(decl, name, argtys, retty, cases, signed, rcomps=None):
    """The .ll of a shader that loads its arguments, and the bytes to load.

    Returns (source, input bytes). The caller assembles the source and gives
    the bytes to air_run.

    `rcomps` are the result components this shader carries back, which is all
    of them for every type but i64: an i64 component needs two of the target's
    four 32-bit lanes, so a v4i64 result is split over two shaders. See
    comp_batches.

    A half result is written to a HALF render target rather than widened into a
    float4. The widening used to happen inside the shader (fpext half to float),
    and NIR then folded the conversion that produced the value into a 32-bit
    one: air.convert.f.f16.u.i32 came out as four cov.u32f32 and not one
    cov.u32f16, so the shader computed f32(x) while the reference computed
    f16(x) and the comparison meant nothing. Ten declarations of the corpus
    return half and every one of them had to be set aside for it.

    half4 as a render target type name is not invented here: Apple's own
    GNR.videoprocessor library, built by the same compiler for the same AIR
    version, has 24 such declarations across 18 fragment entry points --
    !"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4" over a
    define returning <4 x half>. It is not attested by the 456-module corpus,
    which is a statement about how that corpus was collected (.metallib files
    in rootfs, not libraries embedded in dylibs) and not about Metal.
    """
    rty, rn = parse_type(retty)
    outty = "half" if rty == "half" else "float" if rty == "float" else "i32"
    outname = {"half": "half4", "float": "float4"}.get(outty, "int4")
    if rcomps is None:
        rcomps = list(range(rn))

    data = bytearray()
    lines = []
    slots = []
    for i, args in enumerate(cases):
        while len(data) < (i + 1) * SLOTS_PER_CASE * 4:
            data += b"\0\0\0\0"
        argvals = []
        for j, (ty, v) in enumerate(zip(argtys, args)):
            bty, bn = parse_type(ty)
            cslots = lanes_per_comp(bty)
            align = 8 if cslots == 2 else 4
            names = []
            for c in range(bn):
                slot = i * SLOTS_PER_CASE + j * SLOTS_PER_ARG + c * cslots
                b = slot_bytes(bty, v[c])
                data[slot * 4:slot * 4 + len(b)] = b
                nm = "%%l%d_%d_%d" % (i, j, c)
                lines.append("  %sp = getelementptr inbounds i8, "
                             "ptr addrspace(2) %%in, i64 %d" % (nm, slot * 4))
                lines.append("  %s = load %s, ptr addrspace(2) %sp, align %d"
                             % (nm, bty, nm, align))
                names.append(nm)
            if bn == 1:
                argvals.append("%s %s" % (bty, names[0]))
            else:
                prev = "undef"
                for c, nm in enumerate(names):
                    vv = "%%g%d_%d_%d" % (i, j, c)
                    lines.append("  %s = insertelement <%d x %s> %s, %s %s, i32 %d"
                                 % (vv, bn, bty, prev, bty, nm, c))
                    prev = vv
                argvals.append("<%d x %s> %s" % (bn, bty, prev))

        lines.append("  %%c%d = call %s @%s(%s)"
                     % (i, retty, name, ", ".join(argvals)))
        for k in rcomps:
            s_ = "%%c%d" % i
            if rn > 1:
                s_ = "%%e%d_%d" % (i, k)
                lines.append("  %s = extractelement %s %%c%d, i32 %d"
                             % (s_, retty, i, k))
            if rty == "half" and outty == "float":
                lines.append("  %%f%d_%d = fpext half %s to float" % (i, k, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty in ("i1", "i8", "i16") and outty == "i32":
                lines.append("  %%f%d_%d = %s %s %s to i32"
                             % (i, k, "sext" if signed else "zext", rty, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty == "i64" and outty == "i32":
                slots.extend(split64(lines, i, k, s_))
            elif rty == "double" and outty == "i32":
                lines.append("  %%b%d_%d = bitcast double %s to i64" % (i, k, s_))
                slots.extend(split64(lines, i, k, "%%b%d_%d" % (i, k)))
            else:
                slots.append(s_)

    pad = "0xH0000" if outty == "half" else "0.0" if outty == "float" else "0"
    if len(slots) > 4:
        raise AssertionError(
            "%s: %d lanes for a four-lane render target; comp_batches and this "
            "emitter disagree" % (name, len(slots)))
    packed = (slots + [pad] * 4)[:4]
    prev = "undef"
    for i, s_ in enumerate(packed):
        lines.append("  %%v%d = insertelement <4 x %s> %s, %s %s, i32 %d"
                     % (i, outty, prev, outty, s_, i))
        prev = "%%v%d" % i

    return (BUF_SKELETON % dict(decl=decl, outty=outty, outname=outname,
                                body="\n".join(lines)),
            bytes(data), outname)



def fconst(ty, x):
    """A float constant LLVM will read back bit-for-bit."""
    if ty == "float":
        bits = struct.unpack("<Q", struct.pack("<d", f32(x)))[0]
        return "float 0x%016X" % bits
    if ty == "half":
        # LLVM writes a half as the bits of its own 16-bit encoding
        v = f16(x)
        bits = (0x7C00 if v > 0 else 0xFC00) if math.isinf(v) else \
            struct.unpack("<H", struct.pack("<e", v))[0]
        return "half 0xH%04X" % bits
    if ty == "double":
        return "double 0x%016X" % struct.unpack("<Q", struct.pack("<d", x))[0]
    raise ValueError(ty)


def iconst(ty, x):
    w = int(ty[1:])
    return "%s %d" % (ty, wrap(x, w, True))


def const(ty, x):
    return fconst(ty, x) if ty in ("float", "half", "double") else iconst(ty, x)


def emit(decl, name, argtys, retty, cases, tmp, signed, rcomps=None):
    """Build, assemble and evaluate one shader.

    Returns a list of results, one per case, each a list of the components in
    `rcomps` -- all of them unless the caller split them, which it does for an
    i64 result because each of those takes two of the target's four lanes.
    Every component and not just the first: a v4 operation whose fourth
    component was wrong would otherwise pass.
    """
    rty, rn = parse_type(retty)
    # a double is NOT a float target: it goes out as its two 32-bit halves
    # through an int4, which is the only way a 64-bit value fits four 32-bit
    # lanes at all (lanes_per_comp)
    outty = "float" if rty in ("float", "half") else "i32"
    outname = "float4" if outty == "float" else "int4"
    if rcomps is None:
        rcomps = list(range(rn))

    lines = []
    slots = []
    for i, args in enumerate(cases):
        a = []
        for ty, v in zip(argtys, args):
            bty, bn = parse_type(ty)
            if bn == 1:
                a.append(const(bty, v[0]))
            else:
                a.append("<%d x %s> <%s>"
                         % (bn, bty, ", ".join(const(bty, x) for x in v[:bn])))
        lines.append("  %%c%d = call %s @%s(%s)" % (i, retty, name, ", ".join(a)))

        for k in rcomps:
            s_ = "%%c%d" % i
            if rn > 1:
                s_ = "%%e%d_%d" % (i, k)
                lines.append("  %s = extractelement %s %%c%d, i32 %d"
                             % (s_, retty, i, k))
            if rty == "half":
                lines.append("  %%f%d_%d = fpext half %s to float" % (i, k, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty in ("i1", "i8", "i16") and outty == "i32":
                # sext for a signed result: the render target is read back as
                # an unsigned 32-bit word, so a zero-extended -1 would come
                # out as 65535 and read as the translator's mistake rather
                # than this harness's
                lines.append("  %%f%d_%d = %s %s %s to i32"
                             % (i, k, "sext" if signed else "zext", rty, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty == "i64" and outty == "i32":
                # TWO LANES, NOT A trunc. See lanes_per_comp.
                slots.extend(split64(lines, i, k, s_))
            elif rty == "double" and outty == "i32":
                lines.append("  %%b%d_%d = bitcast double %s to i64" % (i, k, s_))
                slots.extend(split64(lines, i, k, "%%b%d_%d" % (i, k)))
            else:
                slots.append(s_)

    # the insertelement below names the type, so a pad is the bare value
    pad = "0.0" if outty == "float" else "0"
    if len(slots) > 4:
        raise AssertionError(
            "%s: %d lanes for a four-lane render target; comp_batches and this "
            "emitter disagree" % (name, len(slots)))
    packed = (slots + [pad] * 4)[:4]
    prev = "undef"
    for i, s_ in enumerate(packed):
        lines.append("  %%v%d = insertelement <4 x %s> %s, %s %s, i32 %d"
                     % (i, outty, prev, outty, s_, i))
        prev = "%%v%d" % i

    src = SKELETON % dict(decl=decl, outty=outty, outname=outname,
                          body="\n".join(lines))
    ll = os.path.join(tmp, "ic.ll")
    bc = os.path.join(tmp, "ic.bc")
    with open(ll, "w") as f:
        f.write(src)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    q = subprocess.run([AIR2NIR, "--eval", bc], capture_output=True)
    for line in q.stdout.decode().splitlines():
        m = re.match(r"\s+out\s+(.*)$", line)
        if m:
            if "did not fold" in m.group(1):
                return None, "did not fold to a constant"
            vals = m.group(1).split()
            # The lanes exactly as --eval printed them: a float for a float
            # target, an unsigned 32-bit word for an integer one. unlane()
            # does the signedness and puts an i64's two words back together,
            # because a half-value cannot be signed before it is whole.
            flat = ([float(v) for v in vals] if outty == "float"
                    else [int(v) for v in vals])
            res = unlane(flat, len(cases), rcomps, rty, signed)
            if res is None:
                return None, ("--eval printed %d lanes, not the %d this shader "
                              "filled" % (len(flat),
                                          len(cases) * len(rcomps)
                                          * lanes_per_comp(rty)))
            return res, None
    err = q.stderr.decode().strip().splitlines()
    return None, (err[0] if err else "no output stored")


# ---------------------------------------------------------------------------
# the check


def base_and_sign(name):
    """'air.clamp.s.v2i16' -> ('clamp', True); 'air.fast_round.f32' -> ('fast_round', None)

    The same split works for llvm.fmuladd.v4f32 -> ('fmuladd', None): both
    vocabularies put the operation first and the type last.
    """
    parts = name.split(".")[1:]
    op = parts[0]
    sign = None
    if len(parts) >= 2 and parts[1] in ("s", "u"):
        sign = parts[1] == "s"
    return op, sign


# Dot products are checked on values whose products and sums are exact, so
# that the accumulation order does not enter into it.
DOT_FLOATS = [0.0, 1.0, -1.0, 2.0, 3.0, -4.0, 0.5, -0.25, 8.0, -16.0]


# A short pool, for the run on real hardware: 300 ms a module here and more on
# the phone, so the full cross product is 12 031 modules and an hour. What the
# GPU run has to establish is that each operation *executes* correctly, not the
# input coverage -- the folding check above already does the grid exhaustively.
# So these are the values that have actually broken something: both zeros, the
# half-way cases that separate round from rint, the largest float below a half,
# and the integer extremes.
#
# THE ORDER OF EVERY POOL BELOW IS LOAD-BEARING: --cases N keeps the first N and
# nothing else, so a value's position decides whether it is ever asked on the
# phone. Do not sort these, and do not add a value at the front: put it after
# the ones that decide something. Getting this wrong does not make a check fail,
# it narrows the question and stays green -- which is how the Adreno's u32 -> f32
# truncation came to be found by luck rather than by design. The suite carried on
# 20 October used --cases 4, which took [0, 1, -1, 7] from the integer pool, and
# -1 read as unsigned happened to be the one value in it that rounding decides.
FEW_FLOATS = [0.0, -0.0, 0.5, -0.5,
              struct.unpack("<f", struct.pack("<I", 0x3EFFFFFF))[0],
              2.5, -2.5, 3.7]
FEW_FLOATS2 = [1.0, -1.0, 0.0, 0.25]
FEW_INTS = [0, 1, -1, 7, -32768, 32767, 2147483647, -2147483648]
# Integer-to-float conversion gets its own pool, because what breaks there is
# not the magnitude but the rounding, and only a value that falls between two
# representable floats exercises it at all. The Adreno truncates u32 -> f32
# (section 4 of docs/air.md), and 0xFFFFFFFF was enough to see that something
# was wrong while saying nothing about which rule is in use. These say which:
#
#   0x00FFFFFF  2^24-1, the last integer every float can hold exactly: every
#               rounding rule must agree here, so a disagreement is not rounding
#   0x01000001  2^24+1, exactly half way between 2^24 and 2^24+2. The lower
#               neighbour has an even mantissa, so half-to-even rounds DOWN here
#               and half-away-from-zero rounds up
#   0x01000003  2^24+3, half way between 2^24+2 and 2^24+4, where the lower
#               neighbour is odd: half-to-even rounds UP. With the line above it,
#               the pair separates to-even from away-from-zero, which one midpoint
#               alone cannot do
#   0x7FFFFFFF  and 0xFFFFFFFF (-1): 1 below a power of two, where the nearest
#               float is 255 away on one side and 1 on the other -- truncation
#               and rounding differ by a whole ulp, which is what was seen
#
# First in the list are the ones that decide a rule, because --cases N keeps the
# first N: the suite carried to the phone used 4, which took [0, 1, -1, 7] and
# so never asked about a midpoint at all.
# A half destination needs its own midpoints, and for the same reason one level
# down: f16 holds integers exactly only to 2048, above which the spacing is 2.
#   2051  half way between 2050 and 2052, whose lower neighbour is odd, so
#         round-to-nearest-even gives 2052 and truncation gives 2050 -- the f16
#         discriminator, the counterpart of 16777219
#   2049  half way between 2048 and 2050, where both rules give 2048: carried to
#         show that a midpoint is not automatically a test
#   2048  the last integer f16 holds exactly
# Without these the four half-destination declarations of the corpus were asked
# only about values that overflow f16, and answered inf to all of them: the
# expectations read [0, inf, inf, inf], which tests saturation and says nothing
# whatever about rounding. They agreed on the Adreno and that agreement was
# empty. The order interleaves the two precisions so that --cases 4 reaches a
# discriminator for each: 0 for sanity, -1 for a whole-ulp f32 difference,
# 16777219 for the f32 rule, 2051 for the f16 rule.
CONV_INTS = [0, -1, 16777219, 2051, 16777217, 2049, 2048, 16777215,
             2147483647, 1, 7, -32768, 32767, -2147483648]
FEW_INTS2 = [1, -1, 0, 7]
FEW_DOT = [0.0, 1.0, -1.0, 2.0, -4.0, 0.5, -0.25, 8.0]


def pool_for(ty, family, few=False, nth=0):
    if few:
        bty, _ = parse_type(ty)
        if bty == "i1":
            return [0, 1]
        if bty.startswith("i"):
            if family == "convert" and bty == "i32":
                return CONV_INTS
            # I64S belongs in the SHORT pool as well as the long one, because
            # the short one is what gets carried to the phone -- the only
            # measurement that can say what the Adreno does -- and without
            # these that suite cannot tell a 64-bit answer from a 32-bit one.
            # Only the first argument's pool: --few gives the others four
            # values and two, so an appended value is unreachable there, and
            # one operand of this shape decides a comparison.
            if bty == "i64" and nth == 0:
                return FEW_INTS + I64S
            return FEW_INTS if nth == 0 else FEW_INTS2
        if family == "dot":
            return FEW_DOT
        return FEW_FLOATS if nth == 0 else FEW_FLOATS2
    return pool_for_full(ty, family)


def pool_for_full(ty, family):
    bty, _ = parse_type(ty)
    if bty == "i1":
        return [0, 1]
    if bty.startswith("i"):
        # The exhaustive pool has 0x7FFFFFFF and -1, both of which rounding
        # decides, but no exact midpoint between two floats -- so it could tell
        # a wrong conversion from a right one and could not tell which rule was
        # in use. The three added here can (see CONV_INTS), and only the convert
        # family pays for them: INTS is the pool for every integer operation,
        # and widening it would grow the whole exhaustive grid for one question
        # that belongs to one family.
        if family == "convert" and bty == "i32":
            return INTS + [16777215, 16777217, 16777219, 2048, 2049, 2051]
        # see I64S: every value of INTS fits in 32 bits, so without these the
        # 64-bit operations are never asked a question a 32-bit answer gets
        # wrong
        if bty == "i64":
            return INTS + I64S
        return INTS
    return DOT_FLOATS if family == "dot" else FLOATS


def inputs_for(argtys, family, few=False, cases=0):
    """The cases to try: one list per argument, holding its components.

    Every component of a vector argument gets a different value. With the same
    value in all of them, a lowering that read the wrong component, or dropped
    one, or swizzled, would return the right answer for the wrong reason --
    which is the mistake this port has already made four times with textures.
    """
    pools = [pool_for(t, family, few, n) for n, t in enumerate(argtys)]
    widths = [parse_type(t)[1] for t in argtys]

    # the whole pool for the first argument, less of it for the others, or a
    # three-argument operation is tens of thousands of shader builds
    # --cases N takes the first N of the first argument's pool and narrows the
    # others with it: that is how a suite small enough to carry to the phone is
    # cut, and the values are still in the order of how dangerous they are
    first = min(cases, len(pools[0])) if cases else len(pools[0])
    limits = [first]
    limits += [min(max(1, first // 2) if cases else (4 if few else 12), len(p))
               for p in pools[1:2]]
    limits += [min(max(1, first // 4) if cases else (2 if few else 6), len(p))
               for p in pools[2:]]

    grid = [()]
    for lim in limits:
        grid = [g + (i,) for g in grid for i in range(lim)]

    cases = []
    for g in grid:
        case = []
        for a, idx in enumerate(g):
            pool, n = pools[a], widths[a]
            case.append([pool[(idx + c) % len(pool)] for c in range(n)])
        cases.append(tuple(case))
    return cases


def showargs(args):
    """One case's arguments: a vector shows every component it was given."""
    out = []
    for v in args:
        f = ",".join("%.9g" % x if isinstance(x, float) else str(x) for x in v)
        out.append(f if len(v) == 1 else "(" + f + ")")
    return " ".join(out)


def ulps(a, b):
    """Distance in float32 steps, for reporting an approximate operation."""
    if math.isnan(a) and math.isnan(b):
        return 0
    if math.isnan(a) or math.isnan(b) or math.isinf(a) or math.isinf(b):
        return 0 if a == b else float("inf")
    ia, ib = to_f32_bits(f32(a)), to_f32_bits(f32(b))
    # a monotonic map of float32 onto the integers: negatives mirror through
    # zero, so adjacent floats are 1 apart. +0 and -0 both land on 0 -- this
    # number says nothing about the sign of a zero, and for an approximate
    # operation that is the right thing to leave out of it
    if ia >> 31:
        ia = -(ia & 0x7FFFFFFF)
    if ib >> 31:
        ib = -(ib & 0x7FFFFFFF)
    return abs(ia - ib)


# ---------------------------------------------------------------------------
# the same operations, run rather than folded
#
# --emit writes a directory of modules whose arguments come from a buffer, the
# bytes for that buffer, and a runner that needs only `for`, `echo` and
# parameter expansion -- the phone's userland is not this machine's. --check
# reads what the runner printed, wherever it ran, and compares it with the
# expectations in the manifest. --run does both here, through softpipe, which
# is already a second engine: softpipe executes TGSI, so a disagreement between
# it and air2nir --eval is NIR's constant folder against nir_to_tgsi and the
# answer is not pre-decided by either.

RUNNER = """#!/bin/sh
# Run every generated module and print its whole output after a marker line.
# Parsing happens on the host (intrincheck.py --check); this stays dumb on
# purpose, because what is here may be a Darwin userland with no sed or paste.
D=`dirname "$0"`
# Where air_run is: the environment first, then one beside the modules (that is
# a host run, which unpacks both into one directory), then the device's own
# /redwood/bin. The fallback is what makes this runnable from the phone's job
# list, which splits a line on blanks and cannot set a variable at all
# (userland/src/init.c, run_command) -- so the job line is "/bin/sh .../run.sh"
# and nothing can be passed to it.
if [ -z "$AIR_RUN" ]; then
	if [ -x "$D/air_run" ]; then
		AIR_RUN=$D/air_run
	else
		AIR_RUN=/redwood/bin/air_run
	fi
fi
echo "runner: $AIR_RUN"
for bc in "$D"/*.bc; do
	id=${bc##*/}
	id=${id%.bc}
	echo "=== $id"
	AIR_RUN_BUFFER="$D/$id.bin" "$AIR_RUN" "$bc" 2>&1
done
"""


JOBS_HEAD = """# Commands run by the launchd job com.redwood.demo, generated by
# tools/air/intrincheck.py --emit. Do not edit: regenerate it.
#
# %(n)d modules over %(decls)d air.* declarations. Each one calls a single operation on
# arguments it reads out of a buffer -- never on immediates, because a shader whose
# arguments are constants is answered by the compiler and the GPU is never asked.
#
# Nothing here says what any of them should return. The expected values stay on the host
# in manifest.json, which is not carried, and `intrincheck.py --check DIR LOG` judges the
# log afterwards -- so the device cannot be made to agree by reading the answer, and this
# file needs no updating when the suite is regenerated.
#
# The first line is the shader M1 is stated against: it takes no input buffer, and
# --disasm makes ir3 print the Adreno instructions it generated. The screen's name proves
# fd_screen_create won; only the disassembly proves the shader went through ir3 and not
# some later software path, and the four floats look identical either way. Expect
#     2.10170319e-18  0.178894654  1.48687698e+16  1.21394141e+33
/redwood/bin/air_run --disasm /System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib clearFragment01
# Then the suite, carried into %(there)s by build.sh EXTRA_SHARE.
"""


def emit_suite(arith, outdir, want, verbose, few=False, cases=0):
    """Write the modules, their inputs, the manifest and the runner."""
    os.makedirs(outdir, exist_ok=True)
    manifest = []
    n = 0
    noref = 0
    for d, name, retty, argtys, op, plain, sign, family in arith:
        if want and not any(w in name for w in want):
            continue
        rn = parse_type(retty)[1]
        cases_l = inputs_for(argtys, family, few, cases)
        # One module per (component group, batch of cases): an i64 result takes
        # two of the target's four lanes, so a v4i64 declaration needs two
        # modules to carry one case's four components. The manifest records
        # which components a module carried, because the judging side cannot
        # work it out from the lane count alone.
        for rcomps, per_shader in comp_batches(retty):
            for k in range(0, len(cases_l), per_shader):
                batch = cases_l[k:k + per_shader]
                expect = []
                keep = []
                for args in batch:
                    try:
                        expect.append(reference(family, plain, name, args, argtys,
                                                retty, True if sign is None else sign))
                        keep.append(args)
                    except (ValueError, OverflowError, ZeroDivisionError):
                        # no reference for this case, so no module for it;
                        # counted so that "N modules over M declarations" can
                        # be read against the pool it was cut from
                        noref += 1
                if not keep:
                    continue
                src, data, outname = emit_buffered(d, name, argtys, retty, keep,
                                                   sign, rcomps)
                ident = "%04d" % n
                ll = os.path.join(outdir, ident + ".ll")
                with open(ll, "w") as f:
                    f.write(src)
                q = subprocess.run(["llvm-as", "-o", os.path.join(outdir, ident + ".bc"), ll],
                                   capture_output=True)
                if q.returncode:
                    print("REFUSED %-34s llvm-as: %s"
                          % (name, q.stderr.decode().strip().splitlines()[0]))
                    continue
                os.remove(ll)
                with open(os.path.join(outdir, ident + ".bin"), "wb") as f:
                    f.write(data)
                manifest.append(dict(id=ident, name=name, retty=retty, rn=rn,
                                     exact=bool(OPS[plain][2] if plain in OPS else True),
                                     op=plain, signed=True if sign is None else bool(sign),
                                     outname=outname, comps=list(rcomps),
                                     cases=[[list(a) for a in c] for c in keep],
                                     expect=expect))
                n += 1

    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f)
    run = os.path.join(outdir, "run.sh")
    with open(run, "w") as f:
        f.write(RUNNER)
    os.chmod(run, 0o755)
    # rc.jobs: the same suite as a job list, for a device with no shell. run.sh
    # above is for this machine; the phone cannot use it, because iOS 16.5's own
    # root carries no sh at all (/bin holds df and ps, nothing else) and the
    # /var/jb/bin/sh that userland/rc.jobs.jb runs belongs to the jailbreak
    # prefix, which a flash-root image does not carry. So every module gets a
    # line of its own and its bytes come from --buffer, which is the one thing
    # init's run_command can express: full paths, blanks between them, no
    # variable and no quoting.
    #
    # The device path is where userland/build.sh EXTRA_SHARE puts this
    # directory, which is /redwood/share and its own name.
    there = "/redwood/share/" + os.path.basename(os.path.normpath(outdir))
    with open(os.path.join(outdir, "rc.jobs"), "w") as f:
        f.write(JOBS_HEAD % dict(n=len(manifest),
                                 decls=len(set(m["name"] for m in manifest)),
                                 there=there))
        for m in manifest:
            f.write("/redwood/bin/air_run --buffer %s/%s.bin %s/%s.bc\n"
                    % (there, m["id"], there, m["id"]))
    print("intrincheck: %d modules in %s, %d declarations, with run.sh, rc.jobs "
          "and manifest.json" % (len(manifest), outdir,
                                 len(set(m["name"] for m in manifest))))
    if noref:
        print("intrincheck: %d cases of the pool are not in the suite: the reference "
              "has no answer for them (out of a destination's range, outside a domain)"
              % noref)
    return 0


NARROW = {"i1", "i8", "i16", "f16", "half"}


def is_narrow(name, retty):
    """Does this declaration touch a type narrower than 32 bits?

    Read off the mangled name, which carries every operand's type, not only
    the result's: air.convert.f.v2f32.s.v2i16 returns floats and takes i16.
    """
    for part in name.split(".") + [parse_type(retty)[0]]:
        if re.sub(r"^v\d+", "", part) in NARROW:
            return True
    return False


def is_number(t):
    """Is this line a number whole, and not a line that merely starts like one?"""
    try:
        float(t)
        return True
    except ValueError:
        return False


def narrowing_cases(m):
    """Of a declaration's cases, how many cannot be answered without rounding.

    Only for a conversion that narrows to half -- from an integer or from f32.
    For those the rounding IS the operation, so a
    pass over values that are all exactly representable in half says nothing
    about it -- and that is not hypothetical. The first run of this suite used
    --cases 8, and the first eight values of the integer pool are small enough
    to be exact in half, so four air.convert.*.f16 declarations "agreed on
    every case" with the rounding never once performed. The pool is ordered for
    f32's interesting values, which are not f16's.
    """
    rty = parse_type(m["retty"])[0]
    if rty != "half":
        return None
    n = 0
    for case in m["cases"]:
        for arg in case:
            for v in arg:
                try:
                    f = float(v)
                except (TypeError, ValueError):
                    continue
                try:
                    if struct.unpack("<e", struct.pack("<e", f))[0] != f:
                        n += 1
                except (OverflowError, struct.error):
                    n += 1      # outside half's range: it must round or overflow
    return n


def parse_runner_log(path):
    """({id: result}, screen) from whatever the runner printed.

    The screen matters: air_run says which one it opened, and softpipe has no
    16-bit types -- nir_to_tgsi asserts on most of them and silently computes
    the rest at 32 bits, which answered 32767 where a half rounds to 32768.
    That is the engine's limit and not the frontend's, so a half operation
    judged on softpipe would be a verdict about nothing.
    """
    out = {}
    screen = None
    ident = None
    block = []
    lines = [l.rstrip("\n") for l in open(path, errors="replace")]
    # A log QEMU produced has every line twice: qemu/run-ios.sh writes the
    # console to two places that both end up here, and the two copies are
    # adjacent. Undoing it needs care, because a suite legitimately prints the
    # same four numbers for two different cases, so no per-line rule is safe.
    # The test is therefore global: the whole file must consist of adjacent
    # equal pairs. That is a property a real log does not have by accident, and
    # the phone's own log (xl run) does not have at all -- it is not doubled.
    if len(lines) >= 2 and len(lines) % 2 == 0 and \
       all(lines[i] == lines[i + 1] for i in range(0, len(lines), 2)):
        lines = lines[0::2]
        print("intrincheck: %s had every line twice, as a QEMU console log does; "
              "read every second one" % os.path.basename(path))
    def finish():
        if ident is None:
            return
        # An id already answered is not replaced. The console log of a phone or
        # QEMU run repeats lines -- qemu/run-ios.sh prints every one twice --
        # and the second copy of a marker opens a block with nothing after it,
        # which would turn an answer into a refusal. A refusal therefore never
        # overwrites an answer, and the first answer stands.
        if out.get(ident, ("",))[0] == "ok":
            return
        text = "\n".join(block)
        i = text.find("came back as")
        if i < 0:
            why = ""
            for line in block:
                if any(w in line for w in ("this harness", "Assertion", "error",
                                           "no gallium", "stores nothing", "no function")):
                    why = line.strip()
                    break
            out[ident] = ("refused", why or (block[-1].strip() if block else "no output"))
            return
        # The values, which air_run prints one per line and then closes with a
        # line of its own ("air_run: drew ..."). That closing line is what ends
        # the reading, not the first line that does not look like a number: a
        # phone's console is shared, and launchd, backboardd and the DRM shim
        # all write to it between one line of ours and the next. Breaking at
        # the first foreign line would silently take three values where four
        # were printed, and a short row reads exactly like a wrong answer. So
        # anything that is not a number is skipped, and only our own next line
        # stops it.
        vals = []
        for line in text[i:].splitlines()[1:]:
            t = line.strip()
            if t.startswith("air_run:") or t.startswith("=== "):
                break
            # A value is a line that is a number and nothing else. Matching a
            # leading digit is not enough: "2026-10-20 07:50:00.641 distnoted
            # [14:221] ..." begins with one, and taking it for a value put a
            # string where an integer was expected and crashed the check --
            # which is the good failure, but on another day it would have been
            # counted as a wrong answer from the GPU.
            if is_number(t):
                vals.append(t)
        out[ident] = ("ok", vals)

    for line in lines:
        if screen is None and " on " in line and line.startswith("air_run: "):
            screen = "softpipe" if "softpipe" in line else line.split(" on ")[-1].strip()
        # A block starts at run.sh's marker, or -- when there was no run.sh,
        # because the device has no shell and every module was a job line of its
        # own -- at air_run's own first line, which names the module it was
        # given. Either way the id comes from the log and not from the order of
        # the blocks in it, so a line lost or repeated anywhere cannot move one
        # module's numbers onto another's name.
        mark = None
        if line.startswith("=== "):
            mark = line[4:].strip()
        else:
            m = re.match(r"^air_run: (?:\S*/)?(\d+)\.bc[ :]", line)
            if m:
                mark = m.group(1)
        if mark is not None:
            finish()
            ident = mark
            block = []
        else:
            block.append(line)
    finish()
    return out, screen


def check_log(outdir, logpath, verbose):
    """Compare a runner log against the manifest. Where it ran is not our
    business; what it computed is."""
    # A log older than the manifest cannot have come from this suite, and
    # comparing the two is worse than useless: the module ids are 0000.. in
    # every suite, so an older log lines up perfectly and every expectation
    # lands on a different operation's numbers. That produced a plausible
    # "8 differ" with lines like "got 1 want 65535" before it was noticed to be
    # nothing at all. Refused rather than warned, because the output of the
    # mistake looks exactly like a finding.
    mani = os.path.join(outdir, "manifest.json")
    if os.path.getmtime(logpath) < os.path.getmtime(mani):
        print("intrincheck: %s is older than %s, so it cannot be this suite's log "
              "(every suite numbers its modules from 0000, so the ids would match "
              "and the operations would not). Re-run the suite, or re-emit it if "
              "the log is the one you mean."
              % (os.path.basename(logpath), os.path.basename(mani)), file=sys.stderr)
        return 1
    with open(mani) as f:
        manifest = json.load(f)
    got, screen = parse_runner_log(logpath)
    # TGSI's registers are 32-bit, so softpipe cannot run an operation on a
    # narrow type: nir_to_tgsi asserts for i1, i8 and i16, and for a half it
    # computes at 32 bits instead and answers 32767 where a half rounds to
    # 32768. Either way the verdict would be about softpipe. ir3 has real
    # 16-bit registers, so these are a question for the phone and stay open.
    narrow_here = screen == "softpipe"
    # And softpipe cannot run a fused multiply-add either, for a reason that is
    # written down in TGSI's own specification rather than guessed at.
    # nir_to_tgsi maps nir_op_ffma to TGSI_OPCODE_MAD (its opcode table in
    # nir_to_tgsi.c), and docs/gallium/tgsi.rst says of MAD: "Perform a * b +
    # c. The implementation is free to decide whether there is an intermediate
    # rounding step or not." tgsi_exec.c's micro_mad takes that permission --
    # `src0->f[0] * src1->f[0] + src2->f[0]` in C, two roundings -- while NIR's
    # own ffma is `fmaf(src0, src1, src2)` (nir_opcodes.py), one rounding, and
    # Metal's fma is OpenCL's, which is C's, which is fused and required to be.
    #
    # air.fma.v4f32 duly differed by exactly one ulp the first time the run
    # stage reached it, at fma(1.5, 0x1.fffffep-2, -0.5): 0x3e7ffffd fused,
    # 0x3e7ffffc with the product rounded first. Folding the same shader agrees
    # with the reference bit for bit, because NIR's folder does the fma. So the
    # disagreement is a verdict about softpipe's interpreter and not about the
    # frontend -- the same shape as the narrow types above -- and it is set
    # aside by name here and left open for the phone, where ir3 has a real mad.
    #
    # fmod is here for the same reason and not for one of its own: its lowering
    # IS an fma (air_fmod in air_intrinsics.c computes fma(-y, trunc(x/y), x),
    # one rounding instead of the two that nir_frem's subtraction costs), so a
    # MAD that rounds the product puts the old two-rounding answer back.
    # Measured and not supposed: folding the same modules leaves air.fmod.f32
    # differing on 1 of 264 cases and running them on softpipe leaves 22, and
    # every one of the extra 21 is the pre-fma value to the bit -- 1.1920929e-07
    # for fmod(1.5, 0x1.fffffep-2) where one rounding gives 8.94069672e-08.
    # Worse than an ulp in one shape: a rounded product can land exactly on the
    # dividend, so fmod(0.5, 0.1) comes back 0 where the answer is 0.0999999940,
    # because the correction that catches a quotient one too large is the one
    # thing a zero result cannot be distinguished from. That is the interpreter's
    # MAD and not the frontend's arithmetic, and on the Adreno mad.f32 is fused.
    fma_here = screen == "softpipe"

    per_decl = {}
    narrow_skipped = set()
    fma_skipped = set()
    missing = 0
    refused = {}
    short = {}
    half_set_aside = set()
    total = 0
    for m in manifest:
        if narrow_here and is_narrow(m["name"], m["retty"]):
            narrow_skipped.add(m["name"])
            continue
        if fma_here and m["op"] in ("fma", "fmuladd", "fmod"):
            fma_skipped.add(m["name"])
            continue
        # A declaration that returns half cannot be judged from a run of
        # air_run, and this is the frontend's harness and not the shader at
        # fault. air_run reads back through R32G32B32A32_FLOAT -- it has no
        # 16-bit target at all -- so a half result is widened at once and NIR
        # folds u2f16 followed by that widening into a single u2f32. The
        # generated Adreno code for air.convert.f.f16.u.i32 is four
        # cov.u32f32 and not one cov.u32f16, and softpipe returns
        # 4.2949673e+09 where a half would give inf. The reference computes
        # f16(x) and the shader computes f32(x), so comparing them says
        # nothing about either.
        #
        # This used to pass unnoticed because every value large enough to show
        # it produced an infinite expectation, and infinities were skipped
        # without being counted: three cases of four skipped, the zero
        # agreeing, and the declaration reported as agreeing on every case.
        # Both halves of the real answer are in place now: air_run has the
        # 16-bit target (PIPE_FORMAT_R16G16B16A16_FLOAT) and emit_buffered
        # declares a half4 render target instead of widening inside the shader,
        # so the value compared is the half the shader actually wrote. A
        # manifest generated before that change still carries float4 shaders,
        # and comparing those would be the old meaningless comparison -- so the
        # set-aside stays for a manifest that says float4 and nothing else.
        if parse_type(m["retty"])[0] in ("half", "f16") and m.get("outname") != "half4":
            half_set_aside.add(m["name"])
            continue
        r = got.get(m["id"])
        if r is None:
            missing += 1
            continue
        if r[0] == "refused":
            refused.setdefault(m["name"], r[1])
            continue
        vals = r[1]
        # a double comes back as two integer lanes and is reassembled by
        # unlane, so it is NOT parsed as a float here
        isf = parse_type(m["retty"])[0] in ("float", "half")
        # A block that answered printed the render target's four components,
        # always four whether the module filled two of them or all four. Any
        # other count means the block was not read whole -- a line of ours lost
        # among the console's other writers, or one seen twice -- and it is
        # reported as that rather than compared. The loop below used to skip a
        # case whose value was simply not there (`idx >= len(flat)`), which is
        # the quiet failure this repository has been bitten by before: cases
        # that vanish instead of failing, and a total that is lower by exactly
        # as much as nobody noticed.
        if len(vals) != 4:
            short.setdefault(m["name"], []).append((m["id"], len(vals)))
            continue
        flat = []
        for v in vals:
            try:
                flat.append(float(v) if isf else int(float(v)))
            except ValueError:
                flat.append(float("nan"))
        rty = parse_type(m["retty"])[0]
        # Which components this module carried, and how its lanes map onto
        # them. A manifest written before the lanes existed has no "comps",
        # and for it every component is in the module in order -- which is
        # what the old reading assumed for an i64 too, and was wrong.
        mcomps = m.get("comps", list(range(m["rn"])))
        unparsed = not isf and not all(isinstance(v, int) for v in flat)
        rows = (None if unparsed
                else unlane(flat, len(m["cases"]), mcomps, rty, m["signed"]))
        if rows is None:
            # a lane that did not parse as a number, or a short row: either way
            # this module was not read and says nothing
            short.setdefault(m["name"], []).append((m["id"], len(vals)))
            continue
        st = per_decl.setdefault(m["name"], dict(exact=m["exact"], n=0, bad=[],
                                                 aside=[], worst=0, worst_case=None,
                                                 denorm=0, op=m["op"], rty=rty))
        for i, args in enumerate(m["cases"]):
            for p, c in enumerate(mcomps):
                exp = m["expect"][i][c]
                # An infinite expectation used to be skipped here along with the
                # denormals, and that skip hid a real disagreement rather than a
                # licensed one. The four half-returning conversions expected
                # [0, inf, inf, inf]; every infinity was skipped, the zero
                # agreed, and three wrong answers were reported as a declaration
                # that agrees on every case. An infinity is as testable as any
                # other value -- the shader has to return one too -- so it is
                # compared now.
                #
                # A denormal is a different matter and still skipped: Metal
                # compiles with air.compile.denorms_disable, so flushing one to
                # zero is permitted and a disagreement there is not a fault.
                # Counted rather than dropped, because that is the distinction
                # this check keeps getting wrong.
                if (isinstance(exp, float) and exp != 0.0
                        and not math.isinf(exp)
                        and abs(exp) < smallest_normal(rty)):
                    st["denorm"] += 1
                    continue
                st["n"] += 1
                total += 1
                g = rows[i][p]
                if st["exact"]:
                    if isinstance(exp, int):
                        same = g == exp
                    else:
                        same = same_float(g, exp, rty)
                    if not same:
                        # the same per-case set-aside the folding stage applies,
                        # so that a run on the phone reads the same way: see
                        # FMOD_ASIDE
                        if st["op"] == "fmod" and fmod_aside(args, c, rty):
                            st["aside"].append((args, c, g, exp))
                        else:
                            st["bad"].append((args, c, g, exp))
                else:
                    u = ulps(g, exp)
                    if u > st["worst"]:
                        st["worst"], st["worst_case"] = u, (args, c, g, exp)

    ok = bad = approx = 0
    aside_cases = {}
    allaside = []
    for name in sorted(per_decl):
        st = per_decl[name]
        if not st["n"]:
            continue
        if st["exact"]:
            if st["bad"]:
                bad += 1
                print("WRONG   %-34s %d of %d cases differ%s"
                      % (name, len(st["bad"]), st["n"],
                         " (and %d more set aside)" % len(st["aside"])
                         if st["aside"] else ""))
                for args, c, g, exp in st["bad"][:4]:
                    fmt = (lambda v: "%.9g" % v) if isinstance(g, float) else str
                    print("          %-34s [%d] got %-16s want %s"
                          % (showargs(args), c, fmt(g), fmt(exp)))
            elif st["aside"] and len(st["aside"]) == st["n"]:
                allaside.append(name)
                print("NOCASE  %-34s all %d of its compared cases are in the "
                      "set-aside, so nothing was decided" % (name, st["n"]))
            elif st["aside"]:
                aside_cases[name] = len(st["aside"])
                print("ASIDE   %-34s %d of %d cases set aside, the other %d agree"
                      % (name, len(st["aside"]), st["n"],
                         st["n"] - len(st["aside"])))
            else:
                ok += 1
                if verbose:
                    print("exact   %-34s %d cases" % (name, st["n"]))
        else:
            approx += 1
            where = ""
            if st["worst_case"] and st["worst"] > 64:
                args, c, g, exp = st["worst_case"]
                where = "   at %s [%d]: %.9g against %.9g" % (showargs(args), c, g, exp)
            print("approx  %-34s %d cases, worst %s ulp from libm%s"
                  % (name, st["n"], st["worst"], where))

    for name in sorted(refused):
        print("refused %-34s %s" % (name, refused[name][:80]))
    print()
    print("intrincheck: %d exactly defined operations agree on every case, %d differ"
          % (ok, bad))
    if aside_cases:
        print("intrincheck: %d more agree on every case they are claimed to, with %d "
              "cases set aside by name (%s)"
              % (len(aside_cases), sum(aside_cases.values()),
                 ", ".join("%s: %d" % (k, aside_cases[k])
                           for k in sorted(aside_cases))))
        print("intrincheck: the set-aside is %s" % FMOD_ASIDE)
    if allaside:
        print("intrincheck: %d declarations had every compared case in the set-aside, "
              "which decides nothing and is counted as a failure: %s"
              % (len(allaside), ", ".join(sorted(allaside))))
    print("intrincheck: %d are approximate, compared with this machine's libm and not counted"
          % approx)
    if refused:
        print("intrincheck: %d declarations were refused by the harness" % len(refused))
    if missing:
        print("intrincheck: %d modules are in the manifest and not in the log" % missing)
    if short:
        n = sum(len(v) for v in short.values())
        print("intrincheck: %d modules printed something other than four values and were "
              "not read: %s" % (n, ", ".join("%s (%s)" % (k, " ".join(
                  "%s:%d" % (i, c) for i, c in v)) for k, v in sorted(short.items()))))
    if narrow_skipped:
        print("intrincheck: %d declarations left out: a narrow type (i1, i8, i16, f16) "
              "and this ran on softpipe, whose registers are 32-bit" % len(narrow_skipped))
    if fma_skipped:
        print("intrincheck: %d left out for computing an fma: nir_to_tgsi sends ffma to "
              "TGSI's MAD, which its own specification lets round the product first, and "
              "softpipe's micro_mad does -- one ulp at fma(1.5, 0x1.fffffep-2, -0.5). "
              "Metal's fma is fused; so is NIR's ffma, which is why folding agrees. fmod "
              "is in this set because its lowering is an fma, and a rounded product undoes "
              "it: folded, air.fmod.f32 has 1 case of 264 in the named set-aside and the "
              "other 263 agree; run on softpipe, 22 differ. A question for the phone, "
              "where mad.f32 is fused (%s)"
              % (len(fma_skipped), ", ".join(sorted(fma_skipped))))
    # How thin a verdict is matters as much as the verdict. "Agrees on every
    # case" over one case is not a statement about the operation, and the four
    # half-returning conversions were exactly that: three of their four cases
    # skipped, agreement resting on the zero. Named here rather than left for
    # someone to work out from the totals.
    thin = sorted(k for k, v in per_decl.items() if v["n"] and v["n"] < 3 and not v["bad"])
    if thin:
        print("intrincheck: %d declarations agree on fewer than 3 cases, which is "
              "agreement about very little: %s"
              % (len(thin), ", ".join("%s (%d)" % (k, per_decl[k]["n"]) for k in thin)))
    # A half-narrowing declaration whose cases never round is a pass about
    # nothing. Summed over the declaration's modules and not per module: a
    # suite splits one declaration across several shaders, most of which
    # legitimately carry only exact values, and the first version of this
    # check named a declaration whenever any one of its modules did -- it
    # fired on a suite that covered the rounding perfectly well.
    blind_total = {}
    for m in manifest:
        if parse_type(m["retty"])[0] != "half" or m["op"] != "convert":
            continue
        if m["name"] not in per_decl or not per_decl[m["name"]]["n"]:
            continue
        blind_total[m["name"]] = blind_total.get(m["name"], 0) + (narrowing_cases(m) or 0)
    blind = sorted(k for k, v in blind_total.items() if v == 0)
    if blind:
        print("intrincheck: %d declarations narrow to half and not one of their cases "
              "needs rounding, so agreement says nothing about the only thing they do: "
              "%s. Emit them with more cases (--cases, or none at all)."
              % (len(blind), ", ".join(blind)))
    if half_set_aside:
        print("intrincheck: %d declarations set aside: they return half, which this suite packs into a "
              "float4 output (an fpext in emit_buffered), so the conversion is widened inside "
              "the shader and the reference describes an operation it does not perform. "
              "air_run has the 16-bit target; the generator is what still needs a half "
              "render target, and the corpus attests no type name for one (%s)"
              % (len(half_set_aside), ", ".join(sorted(half_set_aside))))
    denorm = sum(v["denorm"] for v in per_decl.values())
    if denorm:
        print("intrincheck: %d cases not compared: the result is a denormal and Metal "
              "compiles with air.compile.denorms_disable" % denorm)
    print("intrincheck: %d cases in all, as RUN on %s and not as folded"
          % (total, screen or "an unnamed screen"))
    return 1 if (bad or allaside or missing or short) else 0


def main(argv):
    verbose = False
    want = []
    decls_file = None
    emit_dir = None
    check_pair = None
    run_here = False
    few = False
    ncases = 0
    i = 0
    while i < len(argv):
        if argv[i] == "-v":
            verbose = True
        elif argv[i] == "--decls":
            i += 1
            decls_file = argv[i]
        elif argv[i] == "--emit":
            i += 1
            emit_dir = argv[i]
        elif argv[i] == "--check":
            check_pair = (argv[i + 1], argv[i + 2])
            i += 2
        elif argv[i] == "--run":
            run_here = True
        elif argv[i] == "--few":
            few = True
        elif argv[i] == "--cases":
            i += 1
            ncases = int(argv[i])
        else:
            want.append(argv[i])
        i += 1

    if check_pair:
        return check_log(check_pair[0], check_pair[1], verbose)

    if not os.path.exists(AIR2NIR):
        print("intrincheck: run tools/air/mk-host.sh first", file=sys.stderr)
        return 1

    unread = []
    nlibs = 0
    if decls_file:
        decls = [l.strip() for l in open(decls_file) if l.strip()]
    else:
        libs = []
        for dirpath, _, names in os.walk(os.path.join(ROOT, "ios165/rootfs")):
            for n in names:
                if n.endswith(".metallib"):
                    libs.append(os.path.join(dirpath, n))
        nlibs = len(libs)
        decls, unread = collect_declarations(sorted(libs))
    # Printed BEFORE anything else and on stderr as well as stdout, because
    # the vocabulary decides how much the rest of this check is a check of:
    # a library that did not open takes its operations out of the suite and
    # pushes the pass rate UP. See collect_declarations.
    if unread:
        # on stderr only: the same count goes out on stdout in the summary at
        # the end, and printing it in both places put it through `grep
        # '^intrincheck:'` twice
        print("intrincheck: %d of %d libraries could not be read, so the "
              "operations only they declare are NOT in this run and the pass "
              "count below is an upper bound" % (len(unread), nlibs),
              file=sys.stderr)
        for lib, why in unread:
            # the reason, cut to one short line: what matters here is whether
            # it is a library this frontend cannot parse (a finding) or a
            # binary that could not be executed (a race in a shared tree)
            why = re.sub(r"^air2nir: \S+: ", "", why.strip().splitlines()[0])
            print("unread  %-34s %s" % (os.path.basename(lib), why[:70]))

    arith = []
    skipped = []
    for d in decls:
        m = DECL.match(d)
        if not m:
            continue
        retty, name, args = m.group(1), m.group(2), m.group(3)
        op, sign = base_and_sign(name)
        plain = op[5:] if op.startswith("fast_") else op
        argtys = [a.strip() for a in args.split(",")] if args.strip() else []
        family = ("convert" if plain == "convert"
                  else plain if plain in ("dot", "all", "any")
                  else "op" if plain in OPS or (plain, sign) in IOPS
                  else None)
        if family is None:
            skipped.append((name, "not an operation this check has a reference for"))
            continue
        # the integer min/max/clamp live in their own table and take their
        # arity from the name, not from OPS
        want_args = (OPS[plain][0] if plain in OPS
                     else 3 if plain == "clamp" else 2)
        if family == "op" and want_args != len(argtys):
            skipped.append((name, "%d arguments, the reference takes %d"
                            % (len(argtys), want_args)))
            continue
        arith.append((d, name, retty, argtys, op, plain, sign, family))

    if emit_dir:
        return emit_suite(arith, emit_dir, want, verbose, few, ncases)

    if run_here:
        # the same modules, drawn by whatever screen air_run opens -- softpipe
        # here, the Adreno on the phone
        d = tempfile.mkdtemp(prefix="intrincheck.run.")
        if emit_suite(arith, d, want, verbose, few, ncases):
            return 1
        log = os.path.join(d, "run.log")
        air_run = os.environ.get("AIR_RUN", os.path.join(
            ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))
        if not os.path.exists(air_run):
            print("intrincheck: no air_run at %s" % air_run, file=sys.stderr)
            return 1
        with open(log, "w") as f:
            q = subprocess.run(["sh", os.path.join(d, "run.sh")],
                               env=dict(os.environ, AIR_RUN=air_run),
                               stdout=f, stderr=subprocess.STDOUT)
        if q.returncode:
            print("intrincheck: run.sh exited %d" % q.returncode, file=sys.stderr)
        return check_log(d, log, verbose)

    tmp = tempfile.mkdtemp(prefix="intrincheck.")
    exact_ok = exact_bad = approx = refused = denorm = notnormal = 0
    total_cases = 0
    # cases the reference declined, by the reason it gave: {why: {name: count}}
    declined = {}
    # and cases it has no answer for at all, by the same shape
    noanswer = {}
    # declarations for which not one case was compared
    nocase = []
    # declarations every one of whose disagreements is in a named set-aside,
    # by declaration: {name: cases}
    aside_cases = {}
    for d, name, retty, argtys, op, plain, sign, family in arith:
        if want and not any(w in name for w in want):
            continue
        is_exact = OPS[plain][2] if plain in OPS else True
        rty = parse_type(retty)[0]
        cases = inputs_for(argtys, family)
        bad = []
        aside = []
        worst = 0
        worst_case = None
        n = 0
        # which result components go in one shader, and how many cases fit
        # beside them in a four-lane render target. One group for everything
        # but an i64 result, whose components take two lanes each.
        for rcomps, per_shader in comp_batches(retty):
            if bad is None:
                break
            for k in range(0, len(cases), per_shader):
                batch = cases[k:k + per_shader]
                got, err = emit(d, name, argtys, retty, batch, tmp, sign, rcomps)
                if got is None:
                    refused += 1
                    print("REFUSED %-34s %s" % (name, err))
                    bad = None
                    break
                for j, args in enumerate(batch):
                    try:
                        expv = reference(family, plain, name, args, argtys, retty,
                                         True if sign is None else sign)
                    except Undecided as why:
                        per = declined.setdefault(str(why), {})
                        per[name] = per.get(name, 0) + 1
                        continue
                    except (ValueError, OverflowError, ZeroDivisionError) as why:
                        # A case the reference has no answer for -- out of the
                        # destination's range, outside a domain -- and NOT a
                        # case that passed. It used to be a bare `continue`,
                        # which is the same quiet subtraction from the total
                        # that this file has been bitten by three times: a
                        # declaration can lose most of its cases this way and
                        # still read "agrees on every case".
                        per = noanswer.setdefault(str(why) or type(why).__name__, {})
                        per[name] = per.get(name, 0) + 1
                        continue
                    for p, c in enumerate(rcomps):
                        exp = expv[c]
                        # Apple compiles with air.compile.denorms_disable, so a
                        # denormal result is allowed to be flushed to zero and
                        # this check has nothing to say about it. ldexp is the
                        # only operation here that reaches one from normal inputs.
                        #
                        # ldexp also loses an overflow. NIR's constant expression
                        # for it is
                        #
                        #     dst = ldexpf(src0, src1);
                        #     /* flush denormals to zero. */
                        #     if (!isnormal(dst)) dst = copysignf(0.0f, src0);
                        #
                        # and !isnormal is true of an infinity as well as of a
                        # denormal, so ldexp(0.5, 32767) folds to 0 where Metal,
                        # OpenCL and C all say +inf. The comment names denormals;
                        # the condition catches more than it means to. That is
                        # Mesa's to fix and not this frontend's to work around, so
                        # those cases are left out here and written down in
                        # docs/air.md instead.
                        if (isinstance(exp, float)
                                and (math.isinf(exp)
                                     or (exp != 0.0
                                         and abs(exp) < smallest_normal(rty)))):
                            if plain == "ldexp":
                                notnormal += 1
                                continue
                            if not math.isinf(exp):
                                denorm += 1
                                continue
                        n += 1
                        total_cases += 1
                        g = got[j][p]
                        if is_exact:
                            if isinstance(g, int) and isinstance(exp, int):
                                same = g == exp
                            else:
                                same = same_float(g, exp, rty)
                            if not same:
                                # a disagreement goes into the set-aside only
                                # if THIS case satisfies the condition; see
                                # FMOD_ASIDE
                                if plain == "fmod" and fmod_aside(args, c, rty):
                                    aside.append((args, c, g, exp))
                                else:
                                    bad.append((args, c, g, exp))
                        else:
                            u = ulps(g, exp)
                            if u > worst:
                                worst, worst_case = u, (args, c, g, exp)
        if bad is None:
            continue
        # A DECLARATION NOBODY COMPARED A CASE OF IS NOT A DECLARATION THAT
        # AGREES, and it used to be counted as one: `if bad:` is false over an
        # empty list, so zero comparisons fell into the `else` and incremented
        # the agreeing count. Six declarations were in exactly that state --
        # the float/half conversions whose reference crashed (see ref_convert)
        # -- and the summary called them agreement. Named, counted apart, and
        # fatal, because this is the shape of every wrong green this check has
        # ever had.
        if not n:
            nocase.append(name)
            print("NOCASE  %-34s not one case was compared, so this is not "
                  "agreement" % name)
            continue
        if is_exact:
            if bad:
                exact_bad += 1
                print("WRONG   %-34s %d of %d cases differ%s"
                      % (name, len(bad), n,
                         " (and %d more set aside)" % len(aside) if aside else ""))
                for args, c, g, exp in bad[:4]:
                    fmt = (lambda v: "%.9g" % v) if isinstance(g, float) else str
                    print("          %-34s [%d] got %-16s want %s"
                          % (showargs(args), c, fmt(g), fmt(exp)))
            elif aside and len(aside) == n:
                # EVERY case set aside is agreement about nothing, and the
                # set-aside must not be able to swallow a whole declaration:
                # that is the NOCASE failure wearing a different hat.
                nocase.append(name)
                print("NOCASE  %-34s all %d of its compared cases are in the "
                      "set-aside, so nothing was decided" % (name, n))
            elif aside:
                # every disagreement satisfied the condition, so the operation
                # agrees everywhere it is claimed to and the stage passes --
                # with the count and the reason printed, never implied
                aside_cases[name] = len(aside)
                print("ASIDE   %-34s %d of %d cases set aside, the other %d agree"
                      % (name, len(aside), n, n - len(aside)))
                args, c, g, exp = aside[0]
                print("          %-34s [%d] got %-16s want %s"
                      % (showargs(args), c, "%.9g" % g, "%.9g" % exp))
            else:
                exact_ok += 1
                if verbose:
                    print("exact   %-34s %d cases" % (name, n))
        else:
            approx += 1
            # where the worst case is, not just how bad: a large ulp count on a
            # polynomial approximation is ordinary, and on a quadrant boundary
            # or a sign of zero it is a different thing entirely
            where = ""
            if worst_case and worst > 64:
                args, c, g, exp = worst_case
                where = "   at %s [%d]: %.9g against %.9g" % (
                    showargs(args), c, g, exp)
            print("approx  %-34s %d cases, worst %s ulp from libm%s"
                  % (name, n, worst if worst != float("inf") else "inf", where))

    print()
    # Repeated at the end as well as at the start: every caller of this script
    # reads it through `tail`, and the first copy is exactly the line that got
    # cut off while the pass count stayed green.
    if unread:
        print("intrincheck: %d of %d libraries could not be read, so this run's "
              "vocabulary is incomplete and its pass rate is an upper bound (%s)"
              % (len(unread), nlibs,
                 ", ".join(sorted(os.path.basename(q) for q, _ in unread))))
    print("intrincheck: %d declarations of the corpus have a reference here, %d others do not"
          % (len(arith), len(skipped)))
    print("intrincheck: %d exactly defined operations agree on every case, %d differ"
          % (exact_ok, exact_bad))
    if aside_cases:
        print("intrincheck: %d more agree on every case they are claimed to, with %d "
              "cases set aside by name (%s)"
              % (len(aside_cases), sum(aside_cases.values()),
                 ", ".join("%s: %d" % (k, aside_cases[k])
                           for k in sorted(aside_cases))))
        print("intrincheck: the set-aside is %s" % FMOD_ASIDE)
    print("intrincheck: %d are approximate, compared with this machine's libm and not counted"
          % approx)
    if refused:
        print("intrincheck: %d could not be built or did not fold" % refused)
    if denorm:
        print("intrincheck: %d cases left out: the reference result is denormal and "
              "denorms_disable permits zero" % denorm)
    if notnormal:
        print("intrincheck: %d ldexp cases left out: NIR's constant expression flushes "
              "anything !isnormal, infinities included" % notnormal)
    # A case the reference declines is not a case that passed and not a case
    # that was quietly dropped, so it is named here with the reason -- the
    # whole point of Undecided being an exception that carries one.
    for why in sorted(declined):
        per = declined[why]
        print("intrincheck: %d cases the reference declines to answer -- %s (%s)"
              % (sum(per.values()), why,
                 ", ".join("%s: %d" % (k, per[k]) for k in sorted(per))))
    # Named too, and for the same reason: a case nobody could ask is not a
    # case that agreed. These are the reference's own limits (a conversion out
    # of the destination's range, a domain a function does not have) and not
    # the frontend's, which is why they are a separate line from the ones
    # above and not an error.
    for why in sorted(noanswer):
        per = noanswer[why]
        print("intrincheck: %d cases the reference has no answer for -- %s (%s)"
              % (sum(per.values()), why,
                 ", ".join("%s: %d" % (k, per[k]) for k in sorted(per)[:8])
                 + (", ..." if len(per) > 8 else "")))
    print("intrincheck: %d cases in all" % total_cases)
    if verbose and skipped:
        print()
        for name, why in sorted(set(skipped)):
            print("no ref  %-34s %s" % (name, why))

    # A GREEN RUN THAT COMPARED NOTHING IS THE WORST OUTCOME THIS SCRIPT CAN
    # HAVE, and until this it was reachable: point AIR2NIR at a binary that
    # cannot be executed -- which is what a relink in this shared tree looks
    # like for the second or two it takes -- and every library dropped out,
    # the vocabulary came back empty, and the summary read "0 agree, 0 differ"
    # and exited 0. Two rules close it, and they are different claims:
    #   - nothing was compared, so there is no result either way;
    #   - something could not be EXECUTED (see not_run_at_all), so what was
    #     compared is less than this run believes. A library that is simply
    #     not bitcode is NOT in this set: that is a fact about the library and
    #     would make the check permanently red for the fourteen it is true of.
    if nocase:
        print("intrincheck: %d declarations had NOT ONE case compared, which is not "
              "agreement and is counted as a failure: %s"
              % (len(nocase), ", ".join(sorted(nocase))))
    contaminated = [(p_, w) for p_, w in unread if not_run_at_all(w)]
    if not total_cases:
        print("intrincheck: NOTHING WAS COMPARED, so this is not a pass; %d "
              "libraries went unread" % len(unread), file=sys.stderr)
    if contaminated:
        print("intrincheck: %d libraries could not be EXECUTED for, not merely not "
              "read (%s) -- a relink under the run, not a result"
              % (len(contaminated),
                 ", ".join("%s: %s" % (os.path.basename(p_), w)
                           for p_, w in contaminated[:4])), file=sys.stderr)
    return 1 if (exact_bad or refused or nocase or not total_cases
                 or contaminated) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
