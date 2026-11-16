/*
 * Copyright © 2026 HakiOS contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The air.* functions. There are 125 distinct declarations in the corpus but
 * their names are <operation>.<type suffix>, so this is a table of about 50
 * operations in four families (docs/metal.md 9.2). The suffix is not trusted
 * for anything: the types come from the call's own LLVM types, and the suffix
 * is only what picks the handler.
 *
 * What each name means is not guessed: it is read off Apple's own compiler.
 * AGXCompilerCore carries the whole vocabulary as a table of names at
 * 0x16d082..0x16d400 of the library in this device's shared cache (the list is
 * in docs/air.md section 3.2), and that table distinguishes exactly the
 * functions the Metal Shading Language Specification defines -- round from
 * rint, exp from exp2, fmod from mod, pow from powr. Two of this file's
 * handlers were guesses before that table was read; one of them was wrong.
 */

#include "air_private.h"
#include "nir_format_convert.h"

#include <string.h>

#include "nir_builtin_builder.h"
#include "util/u_math.h"

/* the dot-separated parts of an air.* name, after "air." */
#define AIR_MAX_PARTS 8
/* 48 and not 32: air.normalize_function_constant_predicate's first part is 37
 * characters, and at 32 split_name refused the whole name rather than the
 * operation -- "a name this cannot take apart", which reads like a malformed
 * module and was a buffer. It is the longest part in Apple's own vocabulary
 * list (section 3.2); the longest multi-part name there,
 * air.next_intersection_query.instancing.triangle_data, has no part over 14.
 */
struct air_name {
   unsigned num;
   char part[AIR_MAX_PARTS][48];
};

/* A multiply-add as the driver has one. The Adreno's ir3 has only the unfused fmad
 * (float_mul_add32 says so); handed an ffma it lowers it to a multiply and an add and fuses
 * those back, and on a shader of CoreAnimation's that went on for ever -- the window server's
 * render thread never left nir_opt_loop -- and what was left of it ir3 could not emit
 * ("Unhandled ALU op: ffma"). A driver with a real fused one (softpipe) gets ffma.
 */
static nir_def *
air_fma(nir_builder *b, nir_def *x, nir_def *y, nir_def *z)
{
   const nir_shader_compiler_options *o = b->shader->options;
   unsigned support = x->bit_size == 16 ? o->float_mul_add16 :
                      x->bit_size == 64 ? o->float_mul_add64 : o->float_mul_add32;

   if (!(support & nir_float_muladd_support_has_ffma) &&
       (support & nir_float_muladd_support_has_fmad))
      return nir_fmad(b, x, y, z);
   return nir_ffma(b, x, y, z);
}

static bool
split_name(const char *name, struct air_name *out)
{
   out->num = 0;
   out->part[0][0] = 0;
   const char *p = name + 4; /* past "air." */
   /* "air." with nothing after it names no operation. Without this the loop
    * below never runs, split_name returns true all the same, and the
    * dispatcher compares a part[0] nothing wrote -- which gcc points out as
    * soon as the first comparison is made unconditionally.
    */
   if (!*p)
      return false;
   while (*p && out->num < AIR_MAX_PARTS) {
      const char *dot = strchr(p, '.');
      size_t len = dot ? (size_t)(dot - p) : strlen(p);
      if (len >= sizeof(out->part[0]))
         return false;
      memcpy(out->part[out->num], p, len);
      out->part[out->num][len] = 0;
      out->num++;
      if (!dot)
         return true;
      p = dot + 1;
   }
   return out->num < AIR_MAX_PARTS;
}

static bool
is_signed_part(const char *s)
{
   return !strcmp(s, "s");
}

/* Metal's fmod: the remainder of x/y, with the SIGN OF x, correctly rounded.
 *
 * nir_frem is not that function. Its constant expression is, literally,
 *
 *     src0 - src1 * truncf(src0 / src1)                (nir_opcodes.py)
 *
 * and ir3 lowers it to the same three instructions (ir3_compiler.c sets
 * .lower_fmod). Two things are wrong with the formula, and
 * tools/air/intrincheck.py counted both: of 264 cases of air.fmod.f32 it
 * reported 47 wrong, 25 of them a sign and 22 of them a number.
 *
 *   - THE SIGN OF A ZERO IS LOST. fmod(-0, 5) is -0 and fmod(-1.5, 0.5) is
 *     -0, and the subtraction gives +0 for both: -0 - (0.5 * -0) is -0 - -0,
 *     which is +0. This is the shape of the air.sign bug recorded further
 *     down this file -- a sign of zero that a formula does not carry -- and
 *     it is the half of this that is not about precision at all.
 *   - THE PRODUCT IS ROUNDED BEFORE THE SUBTRACTION. y * trunc(x/y) needs up
 *     to 48 significant bits and a float has 24, so the subtraction is exact
 *     but of the wrong product: fmod(1.5, 0x1.fffffep-2) came out 1.19e-07
 *     against 8.94e-08, one whole ulp of the dividend, and fmod(1e8, 3) came
 *     out 0 where the answer is 1. A number, not a sign.
 *
 * Whose definition is followed here, and why it is Metal's: the Metal Shading
 * Language Specification's table entry for fmod gives the same subtraction
 * formula, but it inherits OpenCL's accuracy table along with it, where fmod
 * is 0 ULP -- an exactly specified function, not a licensed approximation --
 * and OpenCL's edge cases are C99 Annex F's, which say fmod(+-0, y) is +-0.
 * Two references on this machine agree with that reading and none with the
 * formula: this machine's libm answers -0 for fmodf(-0, 0.5) and for
 * fmodf(-1.5, 0.5), and LLVM's own `frem` -- JIT-executed by lli, which is
 * the second engine and not a reading of a document -- answers -0 for both
 * and for frem(-2.5, -0.5). The corpus itself says nothing either way: not
 * one of its 128 libraries contains a single `frem` instruction, because
 * Apple's compiler always emits the call.
 *
 * So:
 *
 *   q = trunc(x / y)
 *   r = fma(-y, q, x)        one rounding, not two
 *   if (|r| >= |y|)   r -= copysign(|y|, r)      q was one too small
 *   if (sign r != sign x) r += copysign(|y|, x)  q was one too large
 *   if (r == 0)       r  = copysign(0, x)        a zero keeps x's sign
 *
 * The fma is the whole of the precision argument: when q is an exact integer
 * the true remainder x - q*y is itself exactly representable (that is why
 * IEEE 754 can require `remainder` to be exact), so ONE rounding of it is no
 * rounding at all. The two corrections are there because fl(x/y) can round
 * across an integer and move trunc by one either way, and because ir3 does
 * not divide: it lowers fdiv to fmul(a, frcp(b)) and frcp is approximate, so
 * a quotient off by one has to be survivable rather than assumed away.
 *
 * WHAT THIS STILL DOES NOT DO, measured and not guessed: q must be exactly
 * representable, which means |x/y| < 2^24 for a float and 2^11 for a half.
 * Above that no correction of a bounded size can help -- fmod(1e8, 1e-8)
 * wants a quotient of 1e16 -- and the exact answer needs an iterated
 * reduction: scale y up by 2^k so the partial quotient stays under 2^23,
 * subtract, repeat. Each pass removes only 23 - 1 bits of the exponent
 * difference, that difference reaches 253 for two normal floats, so twelve
 * passes of about ten instructions each would be the price, and its exactness
 * ON THE ADRENO could not be established from this machine at all for the
 * frcp reason above. It is not done here; section 4 of docs/air.md carries
 * the condition and the measurement.
 *
 * Over the reference pool this leaves 7 of 264 cases differing, every one of
 * them with |x/y| >= 2^24; over 195 906 random pairs of normal floats every
 * single disagreement with this machine's libm has |x/y| >= 2^24, and over
 * 376 438 random pairs whose quotient IS representable there is none.
 */
static nir_def *
air_fmod(nir_builder *b, nir_def *x, nir_def *y)
{
   uint64_t sign_bit = 1ull << (x->bit_size - 1);
   /* The sign of a float is taken and planted with integer bit operations and
    * never with a multiply by zero or a copysign built out of one: this
    * builder runs with exact off (Metal's fast library), and
    * nir_opt_algebraic's inexact rule (('~fmul', a, 0.0), 0.0) would throw
    * away exactly the bit being carried. iand and ior cannot be rewritten by
    * a float rule because they are not float operations.
    */
   nir_def *sx = nir_iand_imm(b, x, sign_bit);
   nir_def *ay = nir_fabs(b, y);
   nir_def *y_as_x = nir_ior(b, ay, sx);              /* copysign(|y|, x) */

   nir_def *q = nir_ftrunc(b, nir_fdiv(b, x, y));
   nir_def *r = air_fma(b, nir_fneg(b, y), q, x);

   /* |r| >= |y|: the quotient was one too small. A NaN fails this comparison
    * (an ordered fge is false for one) and fails the feq against zero at the
    * end; the middle correction may well fire for a NaN, and adding to a NaN
    * gives a NaN, so a NaN comes out of all three unchanged.
    */
   nir_def *y_as_r = nir_ior(b, ay, nir_iand_imm(b, r, sign_bit));
   r = nir_bcsel(b, nir_fge(b, nir_fabs(b, r), ay), nir_fsub(b, r, y_as_r), r);

   /* r's sign is not x's: the quotient was one too large */
   nir_def *wrong_sign = nir_ine(b, nir_iand_imm(b, r, sign_bit), sx);
   r = nir_bcsel(b, nir_iand(b, wrong_sign, nir_fneu_imm(b, r, 0.0)),
                 nir_fadd(b, r, y_as_x), r);

   /* and a zero result is the dividend's zero */
   return nir_bcsel(b, nir_feq_imm(b, r, 0.0), sx, r);
}

/* -- the families ------------------------------------------------------- */

static bool
math_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n, bool *handled)
{
   const char *op = n->part[0];
   unsigned argc = in->num_operands - 1;

   /* the families this file does not handle are named before the arguments
    * are touched, so that their error says so rather than complaining about
    * a texture handle used as a number
    */
   if (strstr(op, "texture") || strstr(op, "_depth_") || !strncmp(op, "sample", 6) ||
       !strncmp(op, "gather", 6) || !strncmp(op, "read_", 5) || !strncmp(op, "write_", 6) ||
       strstr(op, "_lod_")) {
      *handled = false;
      return true;
   }

   /* air.{fast_,}sincos.<type>(x, ptr cosval): Metal's sincos(x, thread T &cosval) returns the
    * sine and stores the cosine through its second argument -- a pointer, so not one of the
    * numbers the families below read (MercuryPoster's fluidity::sdfVertexShader, Darwin 27) */
   if (!strcmp(op, "fast_sincos") || !strcmp(op, "sincos")) {
      *handled = true;
      if (argc != 2)
         return air_error(c, "intrinsic: air.%s with %u arguments, not 2", op, argc);
      nir_def *x = air_get_ssa(c, in->operands[1]);
      struct air_val *p = air_get(c, in->operands[2]);
      if (!x || !p)
         return false;
      if (p->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: air.%s whose second argument is not a pointer it "
                             "can store through", op);
      uint32_t was_exact = c->b.fp_math_ctrl;
      c->b.fp_math_ctrl = 0;
      nir_def *s = nir_fsin(&c->b, x), *co = nir_fcos(&c->b, x);
      c->b.fp_math_ctrl = was_exact;
      if (!air_store_ptr(c, p->ptr, co, 0))
         return false;
      air_set(c, &in->base, air_val_ssa(c, s));
      return true;
   }

   nir_def *a[4] = { 0 };
   for (unsigned i = 0; i < argc && i < 4; i++)
      if (!(a[i] = air_get_ssa(c, in->operands[1 + i])))
         return false;

   bool is_float = air_type_is_float(air_type_scalar(in->base.type));
   /* the sign of an integer operation is the last-but-one part: air.min.s.i32 */
   bool is_signed = n->num >= 3 ? is_signed_part(n->part[1]) : true;
   nir_def *r = NULL;
   *handled = true;

   /* fast math is what every one of these means: Metal's "fast" library */
   uint32_t was_exact = c->b.fp_math_ctrl;
   c->b.fp_math_ctrl = 0;

   if (!strcmp(op, "fast_fabs") || !strcmp(op, "fabs")) {
      r = nir_fabs(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sqrt") || !strcmp(op, "sqrt")) {
      r = nir_fsqrt(&c->b, a[0]);
   } else if (!strcmp(op, "fast_rsqrt") || !strcmp(op, "rsqrt")) {
      r = nir_frsq(&c->b, a[0]);
   } else if (!strcmp(op, "fast_floor") || !strcmp(op, "floor")) {
      r = nir_ffloor(&c->b, a[0]);
   } else if (!strcmp(op, "fast_ceil") || !strcmp(op, "ceil")) {
      r = nir_fceil(&c->b, a[0]);
   } else if (!strcmp(op, "fast_round") || !strcmp(op, "round")) {
      /* round() takes a half away from zero; rint() takes it to even, and
       * Apple's compiler has both air.fast_round and air.fast_rint, so this
       * one is not the even one. (It was nir_fround_even here until the
       * vocabulary table said otherwise.)
       *
       * Truncate, then step one away from zero if the remainder reaches a
       * half. Not trunc(x + copysign(0.5, x)): that addition rounds for an x
       * just under a half and answers 1 where the answer is 0. x - trunc(x)
       * is exact for every float, so this needs no magnitude guard, and it
       * keeps the sign of a negative zero. Checked bit-exact against C's
       * roundf over the halves, the sub-halves, both zeros and 2^24.
       */
      nir_def *t = nir_ftrunc(&c->b, a[0]);
      nir_def *d = nir_fabs(&c->b, nir_fsub(&c->b, a[0], t));
      r = nir_bcsel(&c->b, nir_fge_imm(&c->b, d, 0.5),
                    nir_fadd(&c->b, t, nir_fsign(&c->b, a[0])), t);
   } else if (!strcmp(op, "fast_rint") || !strcmp(op, "rint")) {
      r = nir_fround_even(&c->b, a[0]);
   } else if (!strcmp(op, "fast_trunc") || !strcmp(op, "trunc")) {
      r = nir_ftrunc(&c->b, a[0]);
   } else if (!strcmp(op, "fast_fract") || !strcmp(op, "fract")) {
      r = nir_ffract(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sin") || !strcmp(op, "sin")) {
      r = nir_fsin(&c->b, a[0]);
   } else if (!strcmp(op, "fast_cos") || !strcmp(op, "cos")) {
      r = nir_fcos(&c->b, a[0]);
   } else if (!strcmp(op, "sinpi") || !strcmp(op, "cospi") || !strcmp(op, "tanpi") ||
              !strcmp(op, "fast_sinpi") || !strcmp(op, "fast_cospi") || !strcmp(op, "fast_tanpi")) {
      /* of pi times the argument (CMCapture's BBRNetAnisoGaussian, cospi.f16; SetupAssistant's
       * textFragment, fast_sinpi.f32) */
      char k = op[strncmp(op, "fast_", 5) ? 0 : 5];
      nir_def *x = nir_fmul(&c->b, a[0], nir_imm_floatN_t(&c->b, M_PI, a[0]->bit_size));
      r = k == 's' ? nir_fsin(&c->b, x) : k == 'c' ? nir_fcos(&c->b, x)
                   : nir_fdiv(&c->b, nir_fsin(&c->b, x), nir_fcos(&c->b, x));
   } else if (!strcmp(op, "fast_tan") || !strcmp(op, "tan")) {
      r = nir_fdiv(&c->b, nir_fsin(&c->b, a[0]), nir_fcos(&c->b, a[0]));
   } else if (!strcmp(op, "fast_atan2") || !strcmp(op, "atan2")) {
      /* Mesa's nir_atan2 is right everywhere but the origin, where its own
       * comment says it deliberately pretends 0/0 = 1 because GLSL licenses an
       * implementation to deviate from IEEE at (0, 0). That gives
       * atan2(0, 0) = 3*pi/4. Metal's atan2 is OpenCL's, which keeps the IEEE
       * values:
       *
       *    atan2(+-0, x > 0) = +-0        atan2(+-0, +0) = +-0
       *    atan2(+-0, x < 0) = +-pi       atan2(+-0, -0) = +-pi
       *
       * so a shader taking the angle of a vector that happens to be zero gets
       * 135 degrees instead of 0. That is not a precision deviation of the
       * kind the fast_ prefix allows, so the origin is handled here and the
       * rest left to Mesa.
       *
       * The sign of a zero cannot be had from a float comparison -- -0 == 0 --
       * so the sign bit is read as an integer. A NIR value is a bit vector and
       * ilt reinterprets it; nir_atan2 avoids this only to match the GLSL
       * front-end, which is not a constraint here.
       */
      nir_def *y = a[0], *x = a[1];
      nir_def *origin = nir_iand(&c->b, nir_feq_imm(&c->b, y, 0.0),
                                 nir_feq_imm(&c->b, x, 0.0));
      nir_def *mag = nir_fmul_imm(&c->b,
                                  nir_b2fN(&c->b, nir_ilt_imm(&c->b, x, 0),
                                           y->bit_size),
                                  M_PI);
      nir_def *at_origin =
         nir_bcsel(&c->b, nir_ilt_imm(&c->b, y, 0), nir_fneg(&c->b, mag), mag);
      r = nir_bcsel(&c->b, origin, at_origin, nir_atan2(&c->b, y, x));
   } else if (!strcmp(op, "fast_asin") || !strcmp(op, "asin") ||
              !strcmp(op, "fast_acos") || !strcmp(op, "acos")) {
      /* The inverse sine and cosine, reduced to atan2, which this file
       * already has and which Mesa implements:
       *
       *    asin(x) = atan2(x, sqrt(1 - x*x))
       *    acos(x) = atan2(sqrt(1 - x*x), x)
       *
       * The reduction is exact rather than an approximation of its own, which
       * is why it is preferred to the polynomial in Mesa's SPIR-V front end
       * (build_asin in vtn_glsl450.c): that one's own comment says it "isn't
       * precise enough to meet half-float precision requirements" and names
       * this formula as the alternative it rejected for cost. Cost is the
       * right thing to spend here -- air.fast_acos is called by 183 of
       * Resident Evil 4's modules and the shape of a terminator's lighting
       * depends on it.
       *
       * (1 - x)*(1 + x) and not 1 - x*x: the two are the same number in exact
       * arithmetic and not in a float. Near |x| = 1 the subtraction of two
       * close values loses most of the significand -- at x = 1 - 2^-20 the
       * product form keeps the result to a few ulp where 1 - x*x has none of
       * the low bits left -- and |x| near 1 is exactly where a normal dotted
       * with a light direction spends its time.
       *
       * The edges fall out of atan2 rather than needing a case: the second
       * argument is a square root and so never negative, both forms reach
       * atan2(0, +-1) = {0, pi} at x = +-1, |x| > 1 takes the root of a
       * negative and gives NaN as the domain demands, and neither form can
       * reach atan2(0, 0) -- x and 1 - x*x are not both zero -- so the origin
       * this file corrects for plain atan2 is unreachable from here.
       */
      nir_def *x = a[0];
      nir_def *root = nir_fsqrt(&c->b,
         nir_fmul(&c->b, nir_fsub(&c->b, nir_imm_floatN_t(&c->b, 1.0, x->bit_size), x),
                  nir_fadd_imm(&c->b, x, 1.0)));
      bool is_asin = !strcmp(op, "fast_asin") || !strcmp(op, "asin");
      r = is_asin ? nir_atan2(&c->b, x, root) : nir_atan2(&c->b, root, x);
   } else if (!strcmp(op, "fast_atan") || !strcmp(op, "atan")) {
      /* one argument: the arc tangent of a ratio already taken, which is
       * Metal's atan(y_over_x) and GLSL's atan(y_over_x) both
       */
      r = nir_atan(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sinh") || !strcmp(op, "sinh")) {
      /* 0.5 * (e^x - e^-x), which is Mesa's own lowering of GLSL sinh
       * (vtn_glsl450.c), with the small-argument case taken out of it.
       *
       * A DIFFERENCE OF TWO EXPONENTIALS HAS NO LOW BITS LEFT NEAR ZERO: at
       * x = 1e-8 both e^x and e^-x round to 1.0 in a float, so the formula
       * answers 0 where sinh(1e-8) is 1e-8 -- not an imprecise answer but a
       * wrong one, and the first thing tools/air/intrincheck.py said about
       * this lowering. sinh(x) = x + x^3/6, so below the x where x^3/6 falls
       * under the type's own precision the argument IS the answer, exactly;
       * 2^-12 is that point for a float (x^2/6 < 2^-24) and 2^-5 for a half.
       * Above it the formula keeps a few thousand ulp of relative error near
       * the threshold, which is what a difference of exponentials costs and
       * what the fast_ prefix licenses.
       */
      unsigned bits = a[0]->bit_size;
      nir_def *big = nir_fge_imm(&c->b, nir_fabs(&c->b, a[0]),
                                 bits > 16 ? 0x1p-12 : 0x1p-5);
      r = nir_bcsel(&c->b, big,
                    nir_fmul_imm(&c->b,
                                 nir_fsub(&c->b, nir_fexp(&c->b, a[0]),
                                          nir_fexp(&c->b, nir_fneg(&c->b, a[0]))), 0.5),
                    a[0]);
   } else if (!strcmp(op, "fast_cosh") || !strcmp(op, "cosh")) {
      r = nir_fmul_imm(&c->b, nir_fadd(&c->b, nir_fexp(&c->b, a[0]),
                                       nir_fexp(&c->b, nir_fneg(&c->b, a[0]))), 0.5);
   } else if (!strcmp(op, "fast_tanh") || !strcmp(op, "tanh")) {
      /* (e^x - e^-x) / (e^x + e^-x), with the two guards Mesa's own lowering
       * of GLSL tanh carries and for the same reasons (vtn_glsl450.c):
       *
       *  - the argument is clamped to +-10, above which e^x overflows while
       *    the answer has been 1.0 to the last bit of a float since |x| = 9;
       *  - and the clamp would turn a NaN into 10 and so a NaN into 1, so the
       *    result is taken only where the argument is large enough and handed
       *    back otherwise. That also keeps tanh(-0) = -0, which the division
       *    would give as +0... and this one is EXACTLY the shape of the
       *    air.sign bug recorded further up this file: a sign of zero that
       *    NIR's folder keeps and a backend need not.
       *
       * The threshold is 2^-12 (2^-5 for a half) and not zero, which is where
       * this differs from Mesa's: tanh(x) = x - x^3/3, so below that point
       * the argument is the answer to the last bit, and above it the
       * difference of exponentials loses the low bits the same way sinh's
       * does. With Mesa's threshold of zero, tanh(1e-8) came back 0.
       */
      unsigned bits = a[0]->bit_size;
      nir_def *x = nir_fmin(&c->b, nir_fmax(&c->b, a[0],
                                            nir_imm_floatN_t(&c->b, bits > 16 ? -10.0 : -4.2, bits)),
                            nir_imm_floatN_t(&c->b, bits > 16 ? 10.0 : 4.2, bits));
      nir_def *ex = nir_fexp(&c->b, x), *enx = nir_fexp(&c->b, nir_fneg(&c->b, x));
      uint32_t was = c->b.fp_math_ctrl;
      c->b.fp_math_ctrl = nir_fp_exact;
      nir_def *regular = nir_fge_imm(&c->b, nir_fabs(&c->b, a[0]),
                                     bits > 16 ? 0x1p-12 : 0x1p-5);
      c->b.fp_math_ctrl = was;
      r = nir_bcsel(&c->b, regular,
                    nir_fdiv(&c->b, nir_fsub(&c->b, ex, enx), nir_fadd(&c->b, ex, enx)),
                    a[0]);
   } else if (!strcmp(op, "fast_asinh") || !strcmp(op, "asinh") ||
              !strcmp(op, "fast_acosh") || !strcmp(op, "acosh") ||
              !strcmp(op, "fast_atanh") || !strcmp(op, "atanh")) {
      /* The inverse hyperbolics, as logarithms (MLX's and MLCompute's element-wise kernels):
       *
       *   asinh x = sign(x) ln(|x| + sqrt(x^2 + 1))
       *   acosh x = ln(x + sqrt(x^2 - 1))           (NaN below 1, from the root)
       *   atanh x = ln((1 + x) / (1 - x)) / 2       (+-inf at +-1, NaN past it)
       *
       * with the guards sinh and tanh above have, for the same two reasons: near zero
       * asinh and atanh ARE their argument to the last bit (x - x^3/6, x + x^3/3) while the
       * logarithm of a number near 1 has lost those bits, so below 2^-12 (2^-5 for a half)
       * the argument is the answer; and for a large argument x^2 overflows where the answer
       * is ln(2|x|), which is taken past 2^20 (2^6 for a half).
       */
      /* A half is computed as a float and rounded once at the end: a logarithm taken in half
       * precision of a number near 1 had lost three of the half's own ulp. */
      unsigned bits = 32, out_bits = a[0]->bit_size;
      bool sh = strstr(op, "asinh"), ch = strstr(op, "acosh");
      nir_def *x = nir_f2f32(&c->b, a[0]), *ax = nir_fabs(&c->b, x);
      nir_def *one = nir_imm_floatN_t(&c->b, 1.0, bits);
#define LN(v) nir_fmul_imm(&c->b, nir_flog2(&c->b, (v)), 1.0 / M_LOG2E)
      if (sh || ch) {
         nir_def *v = sh ? ax : x;
         /* (a multiply and an add: ir3 has no fused multiply-add to lower an ffma to) */
         nir_def *v2 = nir_fmul(&c->b, v, v);
         nir_def *root = nir_fsqrt(&c->b, sh ? nir_fadd(&c->b, v2, one) : nir_fsub(&c->b, v2, one));
         nir_def *big = nir_fge_imm(&c->b, v, bits > 16 ? 0x1p20 : 0x1p6);
         nir_def *m = nir_bcsel(&c->b, big, nir_fadd_imm(&c->b, LN(v), M_LN2),
                                LN(nir_fadd(&c->b, v, root)));
         if (sh) {
            m = nir_bcsel(&c->b, nir_flt_imm(&c->b, x, 0.0), nir_fneg(&c->b, m), m);
            r = nir_bcsel(&c->b, nir_fge_imm(&c->b, ax, bits > 16 ? 0x1p-12 : 0x1p-5), m, x);
         } else {
            /* below 1 the root is of a negative number only while x^2 < 1; past -1 it is
             * real again and the logarithm a number, so the domain is said outright */
            r = nir_bcsel(&c->b, nir_flt_imm(&c->b, x, 1.0), nir_imm_float(&c->b, NAN), m);
         }
      } else {
         nir_def *q = nir_fdiv(&c->b, nir_fadd(&c->b, one, x), nir_fsub(&c->b, one, x));
         nir_def *m = nir_fmul_imm(&c->b, LN(q), 0.5);
         r = nir_bcsel(&c->b, nir_fge_imm(&c->b, ax, bits > 16 ? 0x1p-12 : 0x1p-5), m, x);
      }
#undef LN
      r = nir_f2fN(&c->b, r, out_bits);
   } else if (!strcmp(op, "fast_exp") || !strcmp(op, "exp")) {
      /* the natural one: Apple's compiler names exp and exp2 separately */
      r = nir_fexp2(&c->b, nir_fmul_imm(&c->b, a[0], M_LOG2E));
   } else if (!strcmp(op, "fast_exp2") || !strcmp(op, "exp2")) {
      r = nir_fexp2(&c->b, a[0]);
   } else if (!strcmp(op, "fast_exp10") || !strcmp(op, "exp10")) {
      r = nir_fexp2(&c->b, nir_fmul_imm(&c->b, a[0], 3.321928094887362)); /* log2(10) */
   } else if (!strcmp(op, "fast_log") || !strcmp(op, "log")) {
      r = nir_fmul_imm(&c->b, nir_flog2(&c->b, a[0]), 1.0 / M_LOG2E);
   } else if (!strcmp(op, "fast_log2") || !strcmp(op, "log2")) {
      r = nir_flog2(&c->b, a[0]);
   } else if (!strcmp(op, "fast_log10") || !strcmp(op, "log10")) {
      r = nir_fmul_imm(&c->b, nir_flog2(&c->b, a[0]), 0.30102999566398120); /* 1/log2(10) */
   } else if (!strcmp(op, "fast_pow") || !strcmp(op, "pow") ||
              !strcmp(op, "fast_powr") || !strcmp(op, "powr")) {
      /* powr is pow with x >= 0 promised, which is what nir_fpow computes */
      r = nir_fpow(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_fmod") || !strcmp(op, "fmod") ||
              !strcmp(op, "fast_mod") || !strcmp(op, "mod")) {
      /* One lowering for the fast form as well as the plain one. Metal's
       * fast-math does license a lower-precision fmod and does license
       * ignoring the sign of a zero -- 131 of the corpus's 139 fmod
       * declarations are air.fast_fmod.f32, so the cheap form would be legal
       * for nearly all of them -- but a reference that is right for one
       * spelling and relaxed for another is a reference nobody can read, and
       * the extra instructions are about a dozen. If the cost ever matters,
       * the one-step form is nir_frem plus the zero's sign, and the condition
       * under which the two differ is written in air_fmod.
       */
      r = air_fmod(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_divide") || !strcmp(op, "divide")) {
      r = nir_fdiv(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "saturate") || !strcmp(op, "fast_saturate")) {
      r = nir_fsat(&c->b, a[0]);
   } else if (!strcmp(op, "sign") && is_float) {
      /* Metal's sign is OpenCL's: sign(-0) is -0, sign(+0) is +0, sign(NaN)
       * is +0. nir_fsign will not promise that -- its own documentation says
       * that for a zero source "there is a preference for the result to have
       * the same sign, but this is not required (it is required by OpenCL)",
       * and likewise for NaN. A backend is therefore licensed to lose it, and
       * softpipe does: it answered +0 for -0 where NIR's own constant folder
       * answered -0, so folding the shader could never have shown this.
       *
       * Built from comparisons instead, which promise what Metal needs:
       * +1 where positive, -1 where negative, 0 for NaN because neither
       * comparison holds, and a zero handed back as it came so it keeps its
       * sign. No immediate is replicated, so this is the same for a vector.
       */
      nir_def *x = a[0];
      nir_def *pos = nir_b2fN(&c->b, nir_fgt_imm(&c->b, x, 0.0), x->bit_size);
      nir_def *neg = nir_b2fN(&c->b, nir_flt_imm(&c->b, x, 0.0), x->bit_size);
      r = nir_bcsel(&c->b, nir_feq_imm(&c->b, x, 0.0), x,
                    nir_fsub(&c->b, pos, neg));
   } else if (!strcmp(op, "sign")) {
      r = nir_isign(&c->b, a[0]);   /* the float case is above */
   } else if (!strcmp(op, "mix")) {
      /* mix(x, y, a) = x + a * (y - x), and that is what Metal defines */
      r = nir_fadd(&c->b, a[0],
                   nir_fmul(&c->b, a[2], nir_fsub(&c->b, a[1], a[0])));
   } else if (!strcmp(op, "dot")) {
      r = nir_fdot(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_fmin") || !strcmp(op, "fmin") || !strcmp(op, "min")) {
      r = is_float ? nir_fmin(&c->b, a[0], a[1])
                   : (is_signed ? nir_imin(&c->b, a[0], a[1]) : nir_umin(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "fast_fmax") || !strcmp(op, "fmax") || !strcmp(op, "max")) {
      r = is_float ? nir_fmax(&c->b, a[0], a[1])
                   : (is_signed ? nir_imax(&c->b, a[0], a[1]) : nir_umax(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "fast_fmax3") || !strcmp(op, "fmax3") || !strcmp(op, "max3")) {
      /* Darwin 26's Metal: max3/min3/median3 of three, which the uber shader calls */
      r = is_float ? nir_fmax(&c->b, nir_fmax(&c->b, a[0], a[1]), a[2])
                   : (is_signed ? nir_imax(&c->b, nir_imax(&c->b, a[0], a[1]), a[2])
                                : nir_umax(&c->b, nir_umax(&c->b, a[0], a[1]), a[2]));
   } else if (!strcmp(op, "fast_fmin3") || !strcmp(op, "fmin3") || !strcmp(op, "min3")) {
      r = is_float ? nir_fmin(&c->b, nir_fmin(&c->b, a[0], a[1]), a[2])
                   : (is_signed ? nir_imin(&c->b, nir_imin(&c->b, a[0], a[1]), a[2])
                                : nir_umin(&c->b, nir_umin(&c->b, a[0], a[1]), a[2]));
   } else if (!strcmp(op, "fast_fmedian3") || !strcmp(op, "fmedian3") || !strcmp(op, "median3")) {
      /* median = max(min(a, b), min(max(a, b), c)) */
      if (is_float)
         r = nir_fmax(&c->b, nir_fmin(&c->b, a[0], a[1]),
                      nir_fmin(&c->b, nir_fmax(&c->b, a[0], a[1]), a[2]));
      else if (is_signed)
         r = nir_imax(&c->b, nir_imin(&c->b, a[0], a[1]),
                      nir_imin(&c->b, nir_imax(&c->b, a[0], a[1]), a[2]));
      else
         r = nir_umax(&c->b, nir_umin(&c->b, a[0], a[1]),
                      nir_umin(&c->b, nir_umax(&c->b, a[0], a[1]), a[2]));
   } else if (!strcmp(op, "fast_clamp") || !strcmp(op, "clamp")) {
      if (is_float)
         r = nir_fmin(&c->b, nir_fmax(&c->b, a[0], a[1]), a[2]);
      else if (is_signed)
         r = nir_imin(&c->b, nir_imax(&c->b, a[0], a[1]), a[2]);
      else
         r = nir_umin(&c->b, nir_umax(&c->b, a[0], a[1]), a[2]);
   } else if (!strcmp(op, "fast_ldexp") || !strcmp(op, "ldexp")) {
      r = nir_ldexp(&c->b, a[0], nir_i2i32(&c->b, a[1]));
   } else if (!strcmp(op, "fast_fma") || !strcmp(op, "fma") || !strcmp(op, "mad")) {
      r = air_fma(&c->b, a[0], a[1], a[2]);
   } else if (!strcmp(op, "abs")) {
      r = is_float ? nir_fabs(&c->b, a[0]) : nir_iabs(&c->b, a[0]);
   } else if (!strcmp(op, "abs_diff")) {
      r = is_signed ? nir_iabs(&c->b, nir_isub(&c->b, a[0], a[1]))
                    : nir_isub(&c->b, nir_umax(&c->b, a[0], a[1]),
                               nir_umin(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "add_sat")) {
      r = is_signed ? nir_iadd_sat(&c->b, a[0], a[1]) : nir_uadd_sat(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "sub_sat")) {
      r = is_signed ? nir_isub_sat(&c->b, a[0], a[1]) : nir_usub_sat(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "mul_hi")) {
      r = is_signed ? nir_imul_high(&c->b, a[0], a[1]) : nir_umul_high(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "popcount")) {
      /* (NIR's bit_count, like find_lsb and find_msb, answers 32 bits whatever it counts:
       * back to the argument's width, as Metal's answer is) */
      r = nir_u2uN(&c->b, nir_bit_count(&c->b, a[0]), a[0]->bit_size);
   } else if (!strcmp(op, "clz")) {
      /* through the highest set bit, which ir3 has: it has no uclz ("Unhandled ALU op: uclz",
       * Recon3D's ReduceLightChangePerThreadgroup). (width - 1) - msb, and msb is -1 for zero,
       * so a zero counts the whole width, as Metal's clz does, at the argument's own width */
      unsigned w = a[0]->bit_size;
      nir_def *msb = nir_ufind_msb(&c->b, w < 32 ? nir_u2u32(&c->b, a[0]) : a[0]);
      r = nir_u2uN(&c->b, nir_isub(&c->b, nir_imm_int(&c->b, (int)w - 1), msb), w);
   } else if (!strcmp(op, "ctz")) {
      /* Metal's ctz returns the type's width when the value is zero */
      unsigned w = a[0]->bit_size;
      nir_def *lsb = nir_u2uN(&c->b, nir_find_lsb(&c->b, a[0]), w);
      r = nir_bcsel(&c->b, nir_ieq_imm(&c->b, a[0], 0), nir_imm_intN_t(&c->b, w, w), lsb);
   } else if (!strcmp(op, "reverse_bits")) {
      r = nir_bitfield_reverse(&c->b, a[0]);
   } else if (!strcmp(op, "rotate")) {
      r = nir_urol(&c->b, a[0], nir_u2u32(&c->b, a[1]));
   } else if (!strcmp(op, "extract_bits")) {
      r = is_signed ? nir_ibitfield_extract(&c->b, a[0], nir_u2u32(&c->b, a[1]),
                                            nir_u2u32(&c->b, a[2]))
                    : nir_ubitfield_extract(&c->b, a[0], nir_u2u32(&c->b, a[1]),
                                            nir_u2u32(&c->b, a[2]));
   } else if (!strcmp(op, "insert_bits")) {
      r = nir_bitfield_insert(&c->b, a[0], a[1], nir_u2u32(&c->b, a[2]),
                              nir_u2u32(&c->b, a[3]));
   } else if (!strcmp(op, "dfdx") || !strcmp(op, "dfdy") || !strcmp(op, "fwidth")) {
      /* In 32 bits whatever the operand's width: ir3 issues dsx/dsy as TYPE_F32 on whatever
       * register it is given, and a half operand came back as garbage -- fwidth.f16 gave inf and
       * NaN on the Adreno 830 where Apple's GPU gives 0.5 (glasscase h_fwidth, 28 October; and
       * glass_background_sdf_lph, which takes the width of its edge with it). */
      /* And per pixel (fine), as Apple's GPU takes them: ir3's plain dsx/dsy are per quad, and
       * the edges anti-aliased with fwidth came out a few dozen pixels apart (stroke_arc,
       * glass_background) where llvmpipe, which differentiates per pixel, matched Apple. */
      nir_def *x = a[0]->bit_size == 32 ? a[0] : nir_f2f32(&c->b, a[0]);
      if (!strcmp(op, "dfdx"))
         r = nir_ddx_fine(&c->b, x);
      else if (!strcmp(op, "dfdy"))
         r = nir_ddy_fine(&c->b, x);
      else
         r = nir_fadd(&c->b, nir_fabs(&c->b, nir_ddx_fine(&c->b, x)), nir_fabs(&c->b, nir_ddy_fine(&c->b, x)));
      if (r->bit_size != a[0]->bit_size)
         r = nir_f2fN(&c->b, r, a[0]->bit_size);
   } else if (!strcmp(op, "get_simd_width") || !strcmp(op, "get_thread_execution_width") ||
              !strcmp(op, "get_simdgroup_size")) {
      r = nir_u2uN(&c->b, nir_load_subgroup_size(&c->b), air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "get_local_size") || !strcmp(op, "get_global_size") ||
              !strcmp(op, "get_local_id") || !strcmp(op, "get_global_id") ||
              !strcmp(op, "get_group_id") || !strcmp(op, "get_num_groups")) {
      /* OpenCL's names for the grid, one dimension at a time (the camera pipeline's
       * CMImaging kernels ask get_local_size(0) and (1)): the sysval's component,
       * at the width the call returns */
      nir_def *v = !strcmp(op, "get_local_size") ? nir_load_workgroup_size(&c->b)
                 : !strcmp(op, "get_global_size")
                    ? nir_imul(&c->b, nir_load_num_workgroups(&c->b), nir_load_workgroup_size(&c->b))
                 : !strcmp(op, "get_local_id") ? nir_load_local_invocation_id(&c->b)
                 : !strcmp(op, "get_global_id") ? nir_load_global_invocation_id(&c->b, 32)
                 : !strcmp(op, "get_group_id") ? nir_load_workgroup_id(&c->b)
                                               : nir_load_num_workgroups(&c->b);
      r = nir_vector_extract(&c->b, v, nir_u2u32(&c->b, a[0]));
      r = nir_u2uN(&c->b, r, air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "get_local_linear_id")) {
      /* the thread's number in its group, counted x first (CMImaging) */
      r = nir_u2uN(&c->b, nir_load_local_invocation_index(&c->b),
                   air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "get_thread_index_in_simdgroup")) {
      /* the lane, asked for by a call rather than taken as [[thread_index_in_simdgroup]]
       * (RealityCoreRenderer's copyMemory kernels, Darwin 27): the same value as the input */
      r = nir_u2uN(&c->b, nir_load_subgroup_invocation(&c->b), air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "get_simdgroup_index_in_threadgroup")) {
      /* (all three at the width the call returns: `.i16` ones are, and a 32-bit answer
       * met a 16-bit operand in the next urem -- CMImaging's texture-style filters) */
      r = nir_u2uN(&c->b, nir_load_subgroup_id(&c->b), air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "all")) {
      r = nir_ball(&c->b, a[0]);
   } else if (!strcmp(op, "any")) {
      r = nir_bany(&c->b, a[0]);
   } else if (!strcmp(op, "simd_sum")) {
      /* Metal's simd_sum: the sum over the SIMD group, given to every lane.
       * NIR's reduce with cluster_size 0 is that, and ir3 has it
       * (nir_intrinsic_reduce in ir3_compiler_nir.c) and splits a vector into
       * scalars on the way (nir_lower_subgroups with lower_to_scalar).
       *
       * Only the sum, because only the sum occurs: Apple's vocabulary carries
       * sixteen more air.simd_* names (product, min, max, and, or, ballot,
       * shuffle, the prefix scans) and no module of either corpus calls one,
       * so each would be code with no case behind it. A float reduction has no
       * defined order in Metal or in NIR, so nothing here could be checked
       * bit-exactly even on the phone.
       */
      r = nir_reduce(&c->b, a[0],
                     .reduction_op = is_float ? nir_op_fadd : nir_op_iadd);
   } else if (!strncmp(op, "quad_", 5) &&
              (!strcmp(op + 5, "sum") || !strcmp(op + 5, "product") || !strcmp(op + 5, "min") ||
               !strcmp(op + 5, "max") || !strcmp(op + 5, "and") || !strcmp(op + 5, "or") ||
               !strcmp(op + 5, "xor"))) {
      /* A reduction over a quad -- four lanes, 0-3, 4-7, ... -- given to each of them: NIR's
       * clustered reduce with a cluster of 4, which ir3 lowers as it does the whole group's
       * (MLX's quantised matrix-vector kernels, MetalFX). */
      const char *k = op + 5;
      bool sgn = n->num > 1 && !strcmp(n->part[1], "s");
      nir_op rop = !strcmp(k, "sum") ? (is_float ? nir_op_fadd : nir_op_iadd)
                 : !strcmp(k, "product") ? (is_float ? nir_op_fmul : nir_op_imul)
                 : !strcmp(k, "min") ? (is_float ? nir_op_fmin : sgn ? nir_op_imin : nir_op_umin)
                 : !strcmp(k, "max") ? (is_float ? nir_op_fmax : sgn ? nir_op_imax : nir_op_umax)
                 : !strcmp(k, "and") ? nir_op_iand : !strcmp(k, "or") ? nir_op_ior : nir_op_ixor;
      r = nir_reduce(&c->b, a[0], .reduction_op = rop, .cluster_size = 4);
   } else if (!strcmp(op, "simd_and") || !strcmp(op, "simd_or") || !strcmp(op, "simd_xor")) {
      r = nir_reduce(&c->b, a[0], .reduction_op = !strcmp(op, "simd_and") ? nir_op_iand
                                                  : !strcmp(op, "simd_or") ? nir_op_ior
                                                                           : nir_op_ixor);
   } else if (!strcmp(op, "simd_min") || !strcmp(op, "simd_max") || !strcmp(op, "simd_product")) {
      /* the other reductions, which MPSImage does call (the comment above predates it) */
      bool mn = !strcmp(op, "simd_min"), mx = !strcmp(op, "simd_max");
      bool sgn = n->num > 1 && !strcmp(n->part[1], "s");
      nir_op rop = mn ? (is_float ? nir_op_fmin : sgn ? nir_op_imin : nir_op_umin)
                 : mx ? (is_float ? nir_op_fmax : sgn ? nir_op_imax : nir_op_umax)
                      : (is_float ? nir_op_fmul : nir_op_imul);
      r = nir_reduce(&c->b, a[0], .reduction_op = rop);
   } else if (!strcmp(op, "normalize_function_constant_predicate")) {
      /* Metal's compiler stores the predicates that say whether a dependent
       * function constant applies in `__metal_implicit_fc_pred_N` bytes, and
       * puts every one through this before storing it. The name is in Apple's
       * own vocabulary (it sits between air.nextafter and air.pack.snorm1x8 in
       * AGXCompilerCore's list of operation names, section 3.2), but no string
       * or symbol in that binary says what it computes, so the reading here is
       * C's: a byte is a predicate if it is not zero.
       *
       * THE CORPUS CANNOT TELL THAT APART FROM THE IDENTITY, and saying so is
       * the point: every argument it is given in these modules is a
       * `zext i1 ... to i8`, already 0 or 1, and any function that agrees with
       * the identity on {0, 1} gives the same answer. A byte of 2 would
       * discriminate, and nothing produces one here -- so this is a reading
       * that no measurement available on this machine can confirm or refute,
       * and a shader that fed it a wider byte would be the first test of it.
       */
      r = nir_b2iN(&c->b, nir_ine_imm(&c->b, a[0], 0),
                   air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "unpack") && n->num >= 2 &&
              !strcmp(n->part[1], "unorm4x8")) {
      /* four unsigned bytes of a word, each divided by 255. Component 0 is the
       * LOW byte, which is both GLSL's unpackUnorm4x8 and Metal's
       * unpack_unorm4x8_to_float, so NIR's opcode is the operation and not an
       * approximation of it. ir3 has no instruction for it and does not need
       * one: it sets lower_unpack_unorm_4x8, and nir_opt_algebraic turns it
       * into four extract_u8 and a divide.
       */
      r = nir_unpack_unorm_4x8(&c->b, a[0]);
      if (air_nir_bit_size(c, in->base.type) == 16)
         r = nir_f2f16(&c->b, r);
   } else if (!strcmp(op, "unpack") && n->num >= 2 &&
              (!strcmp(n->part[1], "unorm2x16") || !strcmp(n->part[1], "snorm2x16") ||
               !strcmp(n->part[1], "snorm4x8"))) {
      /* the other unpacks Metal has, component 0 the low field as above (VideoToolbox's
       * VTMTSComputeFunction1x1, which a poster extension runs at launch) */
      nir_def *w = nir_u2u32(&c->b, a[0]);
      r = !strcmp(n->part[1], "unorm2x16") ? nir_unpack_unorm_2x16(&c->b, w)
        : !strcmp(n->part[1], "snorm2x16") ? nir_unpack_snorm_2x16(&c->b, w)
                                           : nir_unpack_snorm_4x8(&c->b, w);
      if (air_nir_bit_size(c, in->base.type) == 16)
         r = nir_f2f16(&c->b, r);
   } else if (!strcmp(op, "unpack") && n->num >= 3 && !strcmp(n->part[1], "unorm") &&
              !strcmp(n->part[2], "rgb10a2")) {
      /* 10, 10, 10 and 2 bits from the low end, each over its own maximum */
      nir_def *w = nir_u2u32(&c->b, a[0]);
      static const unsigned width[4] = { 10, 10, 10, 2 }, shift[4] = { 0, 10, 20, 30 };
      nir_def *ch[4];
      for (unsigned i = 0; i < 4; i++)
         ch[i] = nir_fmul_imm(&c->b, nir_u2f32(&c->b, nir_ubitfield_extract(&c->b, w,
                     nir_imm_int(&c->b, shift[i]), nir_imm_int(&c->b, width[i]))),
                     1.0 / ((1u << width[i]) - 1));
      r = nir_vec(&c->b, ch, 4);
      if (air_nir_bit_size(c, in->base.type) == 16)
         r = nir_f2f16(&c->b, r);
   } else if (!strcmp(op, "unpack") && n->num >= 3 && !strcmp(n->part[1], "unorm") &&
              !strcmp(n->part[2], "rgb9e5")) {
      /* three 9-bit mantissas from the low end and a 5-bit exponent shared by them, biased
       * by 15 (the copy kernels of MTLBVHBuilder, ComputeGraph): NIR's own reading of the
       * RGB9E5 format, which is Metal's */
      r = nir_format_unpack_r9g9b9e5(&c->b, nir_u2u32(&c->b, a[0]));
      if (air_nir_bit_size(c, in->base.type) == 16)
         r = nir_f2f16(&c->b, r);
   } else if (!strcmp(op, "pack") && n->num >= 3 && !strcmp(n->part[1], "unorm") &&
              !strcmp(n->part[2], "rgb9e5") && a[0]->num_components == 3) {
      r = nir_format_pack_r9g9b9e5(&c->b, nir_f2f32(&c->b, a[0]));
   } else if (!strcmp(op, "pack") && n->num >= 3 && !strcmp(n->part[1], "unorm") &&
              !strcmp(n->part[2], "rgb10a2") && a[0]->num_components == 4) {
      nir_def *f = nir_fsat(&c->b, nir_f2f32(&c->b, a[0]));
      static const unsigned width[4] = { 10, 10, 10, 2 }, shift[4] = { 0, 10, 20, 30 };
      r = nir_imm_int(&c->b, 0);
      for (unsigned i = 0; i < 4; i++) {
         nir_def *q = nir_f2u32(&c->b, nir_fround_even(&c->b,
            nir_fmul_imm(&c->b, nir_channel(&c->b, f, i), (1u << width[i]) - 1)));
         r = nir_ior(&c->b, r, nir_ishl_imm(&c->b, q, shift[i]));
      }
   } else if (!strcmp(op, "pack") && n->num >= 2 &&
              (!strcmp(n->part[1], "unorm2x16") || !strcmp(n->part[1], "unorm4x8") ||
               !strcmp(n->part[1], "snorm2x16") || !strcmp(n->part[1], "snorm4x8"))) {
      /* Metal's pack_float_to_*: GLSL's packUnorm2x16 and friends, component 0 low */
      nir_def *f = nir_f2f32(&c->b, a[0]);
      r = !strcmp(n->part[1], "unorm2x16") ? nir_pack_unorm_2x16(&c->b, f)
        : !strcmp(n->part[1], "unorm4x8")  ? nir_pack_unorm_4x8(&c->b, f)
        : !strcmp(n->part[1], "snorm2x16") ? nir_pack_snorm_2x16(&c->b, f)
                                           : nir_pack_snorm_4x8(&c->b, f);
   } else if (!strcmp(op, "pack") && n->num >= 3 && !strcmp(n->part[1], "unorm") &&
              !strcmp(n->part[2], "rgb565") && a[0]->num_components == 3) {
      /* Darwin 26 RenderBox's resolve shaders: three channels into one 16-bit word, 5, 6 and
       * 5 bits. Component 0 is the LOW field, as in unorm4x8 -- and the module says so
       * itself: it hands this (b, g, r) for a B5G6R5Unorm target, whose red is the high
       * field, and turns the result into 5-5-5 as ((x >> 1) & 0x7fe0) | (x & 31), which
       * keeps the low 5 bits as they are and drops the low bit of the middle field.
       * Each channel is clamped to [0, 1] and rounded to nearest, Metal's unorm rule.
       */
      nir_def *f = nir_fsat(&c->b, nir_f2f32(&c->b, a[0]));
      static const unsigned width[3] = { 5, 6, 5 }, shift[3] = { 0, 5, 11 };
      r = nir_imm_int(&c->b, 0);
      for (unsigned i = 0; i < 3; i++) {
         nir_def *q = nir_f2u32(&c->b, nir_fround_even(&c->b,
            nir_fmul_imm(&c->b, nir_channel(&c->b, f, i), (1u << width[i]) - 1)));
         r = nir_ior(&c->b, r, nir_ishl_imm(&c->b, q, shift[i]));
      }
      r = nir_u2uN(&c->b, r, air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "convert")) {
      /* air.convert.<dst sign>.<dst type>.<src sign>.<src type> */
      if (n->num < 5) {
         c->b.fp_math_ctrl = was_exact;
         return air_error(c, "intrinsic: %s (a convert without both types)", op);
      }
      const struct air_type *dt = in->base.type;
      const struct air_type *st = in->operands[1]->type;
      unsigned bits = air_nir_bit_size(c, dt);
      bool dst_float = n->part[1][0] == 'f';
      bool src_float = n->part[3][0] == 'f';
      bool src_signed = n->part[3][0] == 's';
      nir_def *x = a[0];
      if (!src_float && air_type_bit_size(air_type_scalar(st)) == 1)
         x = src_signed ? nir_ineg(&c->b, nir_b2i32(&c->b, x)) : nir_b2i32(&c->b, x);
      /* A BOOL DESTINATION IS NOT A CONVERSION. C's rule, which MSL inherits,
       * is `x != 0`, and in NIR a boolean is a 1-bit def -- so asking
       * nir_f2uN for one bit produces f2u1, an opcode ir3 has no instruction
       * for and no lowering of. That is the whole of why
       * variable_blur_frag_{lpf,lph} were the only two modules of QuartzCore's
       * library to reach the backend and die there: "Unhandled ALU op: f2u1",
       * from air.convert.u.i1.f.f32. The comparison is also the right answer
       * rather than a way round the backend: a conversion would have a
       * rounding mode, and bool(0.5) is true, not false.
       */
      if (bits == 1)
         r = src_float ? nir_fneu_imm(&c->b, x, 0.0) : nir_ine_imm(&c->b, x, 0);
      else if (dst_float && src_float)
         r = nir_f2fN(&c->b, x, bits);
      else if (dst_float)
         r = src_signed ? nir_i2fN(&c->b, x, bits) : nir_u2fN(&c->b, x, bits);
      else if (src_float)
         r = n->part[1][0] == 's' ? nir_f2iN(&c->b, x, bits) : nir_f2uN(&c->b, x, bits);
      else
         r = src_signed ? nir_i2iN(&c->b, x, bits) : nir_u2uN(&c->b, x, bits);
   } else {
      *handled = false;
   }

   c->b.fp_math_ctrl = was_exact;
   if (!*handled)
      return true;
   if (!r)
      return false;
   air_set(c, &in->base, air_val_ssa(c, r));
   return true;
}

/* -- function constants ------------------------------------------------- */

/* air.is_function_constant_defined(ptr to the constant's global)
 *
 * Metal's companion to `[[function_constant(n)]]`: a shader may ask whether
 * the pipeline supplied a value for a constant, and guard the code that reads
 * it. The answer is known at translation time -- it is exactly whether the
 * caller passed a value for that index in air_to_nir_options -- so this folds
 * to a constant and the guarded branch folds away with it.
 *
 * It is handled here, before math_op, because its argument is a POINTER and
 * math_op resolves every argument as a number: the four fixed_vert_* modules
 * stopped at "a pointer where a number belongs" with no instruction named,
 * because the failure was in reading the arguments and not in the operation.
 */
static bool
fc_defined_op(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands != 2)
      return air_error(c, "intrinsic: air.is_function_constant_defined of %u arguments",
                       in->num_operands - 1);
   struct air_val *p = air_get(c, in->operands[1]);
   if (!p)
      return false;
   if (p->kind != AIR_VAL_PTR || p->ptr.class != AIR_PTR_FUNCTION_CONSTANT)
      return air_error(c, "intrinsic: air.is_function_constant_defined of something "
                          "that is not a function constant");
   bool defined = false;
   for (unsigned i = 0; c->options && i < c->options->num_constants; i++)
      if (c->options->constants[i].index == p->ptr.binding)
         defined = !c->options->constants[i].undefined;
   air_set(c, &in->base, air_val_ssa(c, nir_imm_bool(&c->b, defined)));
   return true;
}

/* -- atomics ------------------------------------------------------------ */

static bool
atomic_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   /* air.atomic.{global,local}.<op>[.<sign>].<type>(ptr, value?, order,
    * scope, volatile)
    */
   /* air.atomic.fence(mem_flags, order, scope): Metal's atomic_thread_fence, an ordering of
    * memory with no meeting of threads -- the flags as a barrier's (1 device, 2 threadgroup,
    * 4 texture), the scope 1 threadgroup, 2 device, 3 SIMD group */
   if (n->num == 2 && !strcmp(n->part[1], "fence")) {
      uint64_t flags = 1, scope = 2;
      if (in->num_operands > 1)
         air_const_uint(in->operands[1], &flags);
      if (in->num_operands > 3)
         air_const_uint(in->operands[3], &scope);
      nir_variable_mode modes = 0;
      if (flags & 1)
         modes |= nir_var_mem_ssbo | nir_var_mem_global;
      if (flags & 2)
         modes |= nir_var_mem_shared;
      if (flags & 4)
         modes |= nir_var_image;
      if (!modes)
         modes = nir_var_mem_ssbo | nir_var_mem_global | nir_var_mem_shared;
      mesa_scope ms = scope == 1 ? SCOPE_WORKGROUP : scope == 3 ? SCOPE_SUBGROUP : SCOPE_DEVICE;
      nir_barrier(&c->b, .execution_scope = SCOPE_NONE, .memory_scope = ms,
                  .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = modes);
      return true;
   }
   if (n->num < 3)
      return air_error(c, "intrinsic: an atomic without a name");
   const char *where = n->part[1];
   const char *op = n->part[2];
   bool is_signed = n->num >= 5 ? is_signed_part(n->part[3]) : true;

   struct air_val *pv = air_get(c, in->operands[1]);
   if (!pv)
      return false;
   /* on a pointer chosen at run time -- Vista's splat culling counts into one of two buffers
    * loaded out of its argument buffer: the atomic on whichever it is, behind the choice, and
    * the old value from the side that ran */
   if (pv->kind == AIR_VAL_PTR_SELECT && pv->num_elems == 2) {
      struct air_val *regs = NULL;
      struct air_split sp;
      bool has_ret = in->base.type->kind != AIR_TYPE_VOID;
      air_split_begin(c, pv->def, &sp);
      for (unsigned side = 0; side < 2; side++) {
         if (side == 1)
            air_split_else(c, &sp);
         air_set(c, in->operands[1], pv->elems[side]);
         bool ok = atomic_op(c, in, n);
         air_set(c, in->operands[1], pv);
         if (!ok)
            return false;
         if (has_ret)
            regs = air_split_keep(c, air_get(c, &in->base), regs);
      }
      air_split_end(c, &sp);
      if (has_ret)
         air_set(c, &in->base, air_split_load(c, regs));
      return true;
   }
   if (pv->kind != AIR_VAL_PTR)
      return air_error(c, "intrinsic: an atomic on something that is not a pointer");
   struct air_ptr p = pv->ptr;
   /* an atomic member of an argument buffer, in the set's own (device) buffer */
   if (p.class == AIR_PTR_ARGBUF && !air_argbuf_data_ptr(c, &p))
      return false;

   bool shared = !strcmp(where, "local");
   if (shared != (p.class == AIR_PTR_SHARED))
      return air_error(c, "intrinsic: air.atomic.%s on %s memory", where,
                       p.class == AIR_PTR_SHARED ? "threadgroup" : "device");

   nir_def *off = p.offset ? p.offset : nir_imm_int(&c->b, 0);

   /* Compare-exchange, refused before the memory order is read -- because its
    * arguments are in different places and reading them as the others' gave a
    * sentence about the wrong thing. Its signature, decoded from the call
    * sites so that implementing it needs no second reading:
    *
    *   i32 air.atomic.<global|local>.cmpxchg.weak.i32(
    *          ptr object, ptr expected, i32 desired,
    *          i32 success_order, i32 failure_order, i32 scope, i1 volatile)
    *
    * so the order this used to read as argument 3 is the DESIRED VALUE, which
    * is not a constant, and 4 modules of Resident Evil 4 were told "with a
    * memory order that is not a constant" about an operation this does not
    * implement at all.
    *
    * THE RESULT IS THE OBJECT'S OLD VALUE and not a boolean, which is read off
    * what the callers do with it rather than from the name: Metal's
    * atomic_compare_exchange_weak_explicit returns bool, and every call site
    * here instead feeds the result to `icmp ne i32 %expected, %result` and
    * then re-loads *expected -- the weak-CAS retry loop HLSL's
    * InterlockedCompareExchange compiles to. A lowering is three operations
    * (load *expected, nir_atomic_op_cmpxchg, store the old value back through
    * `expected`, whose conditional-on-failure store may be unconditional
    * because on success it writes the value already there), and what stops it
    * is not the operation but the check: nothing in tools/air/atomiccheck.py
    * can make a wrong compare-exchange show, and an atomic that is only
    * compiled is the one kind this project will not claim.
    */
   /* ... and now taken, with tools/air/atomiccheck.py's third kernel to show it (a sum built by
    * a compare-exchange loop in every thread, which a lost or doubled exchange cannot add up
    * to). Darwin 27's form has one more i32 than the one above -- MLCompute's
    *   i32 air.atomic.global.cmpxchg.weak.i32(ptr as(1), ptr, i32, i32 0, i32 0, i32 2, i32 0, i1)
    * -- and both have the two orders at operands 4 and 5, which must be relaxed as for the rest. */
   if (!strcmp(op, "cmpxchg")) {
      uint64_t so, fo;
      if (in->num_operands < 7 || !air_const_uint(in->operands[4], &so) ||
          !air_const_uint(in->operands[5], &fo))
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg whose memory orders are not "
                             "constants", where);
      if (so != 0 || fo != 0)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg with memory orders %u and %u; "
                             "only relaxed (0) is translated", where, (unsigned)so, (unsigned)fo);
      struct air_val *ev = air_get(c, in->operands[2]);
      nir_def *desired = air_get_ssa(c, in->operands[3]);
      if (!ev || !desired)
         return false;
      if (ev->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg whose expected value is not "
                             "behind a pointer", where);
      const struct air_type *vt = in->operands[3]->type;
      nir_def *expected = air_load_ptr(c, ev->ptr, vt, 4);
      if (!expected)
         return false;
      nir_def *was;
      if (shared) {
         was = nir_shared_atomic_swap(&c->b, desired->bit_size, off, expected, desired,
                                      .atomic_op = nir_atomic_op_cmpxchg);
      } else if (p.class == AIR_PTR_SSBO) {
         was = nir_ssbo_atomic_swap(&c->b, desired->bit_size, nir_imm_int(&c->b, p.binding), off,
                                    expected, desired, .atomic_op = nir_atomic_op_cmpxchg);
      } else {
         return air_error(c, "intrinsic: an atomic on a pointer that is not a buffer");
      }
      /* what was there goes back through `expected`; on success that is what it already held */
      if (!air_store_ptr(c, ev->ptr, was, 4))
         return false;
      if (in->base.type && in->base.type->kind != AIR_TYPE_VOID)
         air_set(c, &in->base, air_val_ssa(c, was));
      return true;
   }

   /* Metal 3.2's atomic_wait and atomic_notify (MPSCore's single_pass_scan waits for the group
    * before it to publish its sum): wait(ptr, old, order, scope, ...) returns the value once it
    * is no longer `old`, which here is a loop reading it until then -- the notify a waiter would
    * be woken by is then nothing, the waiter seeing the store itself. An acquiring wait orders
    * what follows after the value it saw. */
   if (!strcmp(op, "notify"))
      return true;
   if (!strcmp(op, "wait")) {
      if (in->num_operands <= 2)
         return air_error(c, "intrinsic: air.atomic.%s.wait with no value", where);
      nir_def *old = air_get_ssa(c, in->operands[2]);
      if (!old)
         return false;
      struct air_loop lp;
      air_loop_head(c, &lp);
      nir_def *now = air_load_ptr(c, p, in->base.type, 0);
      if (!now)
         return false;
      air_loop_test(c, &lp, nir_ieq(&c->b, now, old));
      air_loop_end(c, &lp);
      uint64_t order = 0;
      if (in->num_operands > 3 && air_const_uint(in->operands[3], &order) && order != 0)
         nir_barrier(&c->b, .execution_scope = SCOPE_NONE,
                     .memory_scope = strcmp(where, "local") ? SCOPE_DEVICE : SCOPE_WORKGROUP,
                     .memory_semantics = NIR_MEMORY_ACQ_REL,
                     .memory_modes = strcmp(where, "local") ? nir_var_mem_ssbo | nir_var_mem_global
                                                            : nir_var_mem_shared);
      air_set(c, &in->base, air_val_ssa(c, now));
      return true;
   }

   /* The memory order, which this lowering is right for exactly one value of.
    * That argument is Metal's memory_order, and every atomic in the corpus
    * passes 0 -- memory_order_relaxed. A relaxed load or store of an aligned
    * 32-bit word is a plain load or store, which is what happens below, and a
    * relaxed read-modify-write needs no barrier either. A stronger order would
    * need one that is not emitted, so rather than ignore the argument and be
    * quietly wrong on a shader that asks for more, refuse it by name. Read
    * through air_const_uint because a zero arrives as CST_CODE_NULL about as
    * often as as an integer.
    */
   unsigned iorder = !strcmp(op, "load") ? 2 : 3;
   uint64_t order;
   if (in->num_operands <= iorder ||
       !air_const_uint(in->operands[iorder], &order))
      return air_error(c, "intrinsic: air.atomic.%s.%s with a memory order that is "
                          "not a constant", where, op);
   if (order != 0)
      return air_error(c, "intrinsic: air.atomic.%s.%s with memory_order %u; only "
                          "relaxed (0) is translated, and a stronger one needs a "
                          "barrier this does not emit", where, op, (unsigned)order);

   if (!strcmp(op, "load")) {
      nir_def *r = air_load_ptr(c, p, in->base.type, 0);
      if (!r)
         return false;
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   if (!strcmp(op, "store")) {
      nir_def *v = air_get_ssa(c, in->operands[2]);
      return v && air_store_ptr(c, p, v, 0);
   }

   nir_atomic_op aop;
   bool negate = false;   /* sub: an add of the negation, as NIR has no atomic subtract */
   if (!strcmp(op, "add")) aop = nir_atomic_op_iadd;
   else if (!strcmp(op, "sub")) { aop = nir_atomic_op_iadd; negate = true; }
   else if (!strcmp(op, "min")) aop = is_signed ? nir_atomic_op_imin : nir_atomic_op_umin;
   else if (!strcmp(op, "max")) aop = is_signed ? nir_atomic_op_imax : nir_atomic_op_umax;
   else if (!strcmp(op, "and")) aop = nir_atomic_op_iand;
   else if (!strcmp(op, "or")) aop = nir_atomic_op_ior;
   else if (!strcmp(op, "xor")) aop = nir_atomic_op_ixor;
   else if (!strcmp(op, "xchg")) aop = nir_atomic_op_xchg;
   else return air_error(c, "intrinsic: atomic %s", op);

   nir_def *v = air_get_ssa(c, in->operands[2]);
   if (!v)
      return false;
   if (negate)
      v = nir_ineg(&c->b, v);
   nir_def *r;
   if (shared) {
      r = nir_shared_atomic(&c->b, v->bit_size, off, v, .atomic_op = aop);
   } else if (p.class == AIR_PTR_SSBO) {
      r = nir_ssbo_atomic(&c->b, v->bit_size, nir_imm_int(&c->b, p.binding), off, v,
                          .atomic_op = aop);
   } else {
      return air_error(c, "intrinsic: an atomic on a pointer that is not a buffer");
   }
   if (in->base.type && in->base.type->kind != AIR_TYPE_VOID)
      air_set(c, &in->base, air_val_ssa(c, r));
   return true;
}

/* -- barriers ----------------------------------------------------------- */

static bool
barrier_op(struct air_ctx *c, const struct air_inst *in, mesa_scope scope)
{
   /* air.wg.barrier(i32 flags, i32 scope): the flags say which memory is
    * ordered. 2 is threadgroup memory, 1 device, 4 texture; the corpus only
    * ever passes 2 with scope 1 (the threadgroup).
    */
   uint64_t flags = 2;
   if (in->num_operands > 1)
      air_const_uint(in->operands[1], &flags); /* left alone when not readable */

   nir_variable_mode modes = 0;
   if (flags & 1)
      modes |= nir_var_mem_ssbo | nir_var_mem_global;
   if (flags & 2)
      modes |= nir_var_mem_shared;
   if (flags & 4)
      modes |= nir_var_image;
   if (!modes)
      modes = nir_var_mem_shared;

   nir_barrier(&c->b, .execution_scope = scope,
               .memory_scope = scope, .memory_semantics = NIR_MEMORY_ACQ_REL,
               .memory_modes = modes);
   return true;
}

/* -- fragment flow control ---------------------------------------------- */

/* air.discard_fragment()
 *
 * NIR'S `demote` AND NOT NIR'S `terminate`, and that choice is the whole of
 * this function. The two differ in what the thread does afterwards: terminate
 * ends the invocation, so the lane stops contributing to its quad's
 * derivatives and performs no further memory writes, while demote turns it
 * into a HELPER invocation -- it keeps executing, its framebuffer and memory
 * writes are dropped, and it still supplies the neighbour values that a
 * derivative or an implicit-level-of-detail sample in the OTHER three lanes
 * of the quad needs.
 *
 * It is a real distinction in NIR and in every backend that has one: ir3
 * keeps the two apart in its own IR (ir3_compiler_nir.c emits ir3_DEMOTE
 * against ir3_KILL, and they carry different scheduling barriers), and
 * nir_to_tgsi sends them to TGSI's DEMOTE and KILL, which are separate
 * opcodes. ONE PLACE COLLAPSES THEM, AND IT IS WORTH KNOWING ABOUT: the a6xx
 * instruction encoder maps DEMOTE onto the hardware's KILL --
 * `} else if (instr->opc == OPC_DEMOTE) { return OPC_KILL; }` in
 * src/freedreno/isa/encode.c -- and the ISA description has no demote bitset
 * at all (ir3-cat0.xml carries `kill` and nothing beside it). So on this GPU,
 * with this Mesa, both come out as one instruction and the helper lanes Metal
 * promises are not delivered by the hardware path today. That is a gap to
 * name, not a reason to translate this as `terminate`: terminate would lose
 * the same lanes and would be wrong on a backend that does tell them apart.
 * What it costs is measurable only on the phone -- a shader that discards and
 * then takes a derivative -- and that is written as a job rather than assumed.
 *
 * ONE THING IS NOT A DIFFERENCE BETWEEN THE TWO, and saying so is the
 * correction of a sentence that used to stand here: stores. NIR's own
 * definition of demote (nir_intrinsics.py) is "Any stores to memory after
 * this instruction are suppressed and the fragment does not write outputs to
 * the framebuffer. Unlike discard, demote needs to ensure that derivatives
 * will still work for invocations that were not demoted." So demote drops the
 * lane's later writes as well -- it differs from terminate in the derivatives
 * and in the lane going on executing, not in the memory. Whether METAL drops
 * them is a third question, and nothing on this machine answers it: no header
 * on the device says, and a corpus cannot show it. If Metal keeps them, both
 * of NIR's intrinsics are wrong about it by the same amount.
 *
 * Nothing in a corpus COUNT can decide it -- both translate, both validate,
 * both compile -- so it is decided from two independent readings of what
 * Metal means, and both say the thread lives:
 *
 *  1. The shaders themselves. Resident Evil 4's `PS_DrawByTiles` calls
 *     air.discard_fragment at the ninth instruction of a 1 800-line function
 *     and then calls `air.simd_is_helper_thread()` EIGHT TIMES further down
 *     it. A terminated thread cannot be asked whether it is a helper thread;
 *     a demoted one answers yes, which is the only reading under which that
 *     shader means anything. 24 of the library's 1 288 discard-calling modules
 *     call that function.
 *  2. Apple's own declaration of it. `llvm-dis` of that module reads
 *     `declare void @air.discard_fragment() local_unnamed_addr #8` with
 *     `attributes #8 = { nounwind }` -- and NOT `noreturn`. LLVM is entitled
 *     to delete everything after a call it is told does not return, and
 *     Apple's compiler kept the rest of the function; the call is followed by
 *     `br` and not `unreachable` in 73 of 85 call sites sampled. A discard
 *     that killed the thread would be a noreturn call.
 *
 * And the cost of getting it wrong is not hypothetical here: of 85 sampled
 * call sites, 45 are followed in the same function by an air.sample_* with an
 * implicit level of detail and 6 by air.dfdx, both of which read the other
 * lanes of the quad. Under `terminate` those neighbours would read a dead
 * lane.
 *
 * uses_discard is set by hand because nir_shader_gather_info does not run on
 * this path (ir3 calls it only for a binning variant), exactly as the
 * framebuffer-fetch flags are set in air_to_nir.c. It is not bookkeeping: it
 * is what nir_to_tgsi_info turns into `uses_kill`, which is what a Gallium
 * driver reads before it allows an early depth test, and what freedreno reads
 * before it writes LRZ. A discarding shader whose interface says it discards
 * nothing gets its depth written for pixels it threw away.
 */
static bool
discard_op(struct air_ctx *c, const struct air_inst *in)
{
   if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
      return air_error(c, "intrinsic: air.discard_fragment in a %s shader",
                       mesa_shader_stage_name(c->nir->info.stage));
   nir_demote(&c->b);
   c->nir->info.fs.uses_discard = true;
   return true;
}

/* -- SIMD groups and quads ----------------------------------------------- */

/* air.simd_* and air.quad_*: Metal's cross-lane operations.
 *
 * THE SIMD-GROUP HERE IS THE IMPLEMENTATION'S, NOT A 32-LANE ONE, and that is
 * the only way these can be translated honestly. Metal does not fix the width
 * -- a shader asks for it with air.get_simd_width (this file answers with
 * nir_load_subgroup_size) and a host asks MTLComputePipelineState for
 * threadExecutionWidth -- and it is 32 on every Apple GPU, which is why a
 * shader compiled for one may have the number built into it. This hardware's
 * is 64 or 128: `threadsize_base` is 64 for the A635 this port targets and ir3
 * doubles it per variant in the compute and fragment stages
 * (ir3_shader_get_subgroup_size, IR3_SINGLE_OR_DOUBLE).
 *
 * So the width a translated shader is TOLD and the width its votes are taken
 * over are the same number, whatever that number is: a shader that reads
 * air.get_simd_width and works from it is right, and a shader with 32 written
 * into it is wrong -- as it would be on any Metal implementation whose
 * SIMD-group is not 32 wide. Each of these leaves a note saying so, because it
 * is a thing the hardware cannot do exactly rather than a thing this frontend
 * chose.
 *
 * One more thing decides whether that consistency holds, and it is a property
 * of the GPU and not of this file: ir3 declares a fragment or vertex
 * subgroup to be ONE LANE and lowers a vote to its own argument on a device
 * without `has_getfiberid` (ir3_nir.c: `options.subgroup_size = 1`,
 * `lower_vote_trivial`). The A635 has it -- it is an a6xx gen4, and
 * freedreno_devices.py sets the flag for gen4 and above -- so the votes here
 * are real in every stage, and air.get_simd_width answers with the real width
 * in the same stages. On an a6xx below gen4 both would read 1, which is still
 * self-consistent and still not Metal's 32.
 *
 * air.simd_ballot is the one that cannot be translated at all, and it is
 * refused by name below rather than noted: its result is a 64-bit mask with
 * one bit per lane, and a lane count this frontend cannot bound at 64. A
 * popcount of a truncated mask is a smaller number and not a worse one.
 *
 * The QUAD operations are a different matter and are exact: a quad is four
 * consecutive lanes on both machines -- it is what a fragment derivative is
 * computed over -- so a swap inside one has the same meaning whatever the
 * SIMD-group's width is, and nir_lower_subgroups leaves the quad intrinsics
 * alone for ir3 (no lower_quad, and lower_vote_trivial does not reach them).
 */
static bool
simd_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n,
        bool *handled)
{
   const char *op = n->part[0];

   /* THE NAME IS DECIDED BEFORE AN ARGUMENT IS TOUCHED, which is the same rule
    * math_op states above it and fc_defined_op exists for -- and getting it
    * wrong here cost 46 modules of the 456 for the length of one commit. This
    * function used to resolve its first argument before asking whether the
    * name was one of its own, and every texture operation passes a texture
    * HANDLE as that argument: air.sample_texture_2d came back "instruction: a
    * pointer where a number belongs", which reads like a broken module and
    * names no operation at all. The count was the only visible symptom and it
    * was briefly attributed to somebody else's commit.
    */
   if (strncmp(op, "simd_", 5) && strncmp(op, "quad_", 5) &&
       strcmp(op, "get_sample_position") && strncmp(op, "simdgroup_matrix_8x8_", 21)) {
      *handled = false;
      return true;
   }
   *handled = true;

   /* The rest of the 8x8 simdgroup matrix operations, on the two elements a thread holds (row fm,
    * columns fn and fn + 1, as the multiply below lays them out):
    *   init_diag(x): x on the diagonal, 0 elsewhere; init_filled(x): x everywhere;
    *   load(p, shape, stride, origin) and store(m, p, shape, stride, origin): element (r, c) at
    *   p + (origin.x + c) stride.x + (origin.y + r) stride.y elements -- stride is (1, the
    *   elements per row) for simdgroup_load(..., elements_per_row), the two swapped for a
    *   transposed one, and shape (elements per row, 8) is not needed to find an element.
    * MPS's MatrixMultiply and fully connected kernels, Espresso's convolutions. */
   if (!strncmp(op, "simdgroup_matrix_8x8_", 21) &&
       strcmp(op, "simdgroup_matrix_8x8_multiply_accumulate")) {
      const char *what = op + 21;
      nir_def *self = nir_load_subgroup_invocation(&c->b);
      /* fm = 4 l4 + 2 l2 + l1, fn = 4 l3 + 2 l0 */
      nir_def *fm = nir_ior(&c->b, nir_iand_imm(&c->b, nir_ushr_imm(&c->b, self, 2), 4),
                            nir_iand_imm(&c->b, nir_ushr_imm(&c->b, self, 1), 3));
      nir_def *fn = nir_ior(&c->b, nir_iand_imm(&c->b, self, 8),
                            nir_ishl_imm(&c->b, nir_iand_imm(&c->b, self, 1), 1));
      if (!strcmp(what, "init_diag") || !strcmp(what, "init_filled")) {
         const struct air_type *rt = in->base.type;
         nir_def *x = air_get_ssa(c, in->operands[1]);
         if (!x || !air_is_simd_matrix(rt))
            return x ? air_error(c, "intrinsic: air.%s to a value that is not a matrix", n->part[0])
                     : false;
         nir_def *e[2];
         for (unsigned j = 0; j < 2; j++) {
            e[j] = x;
            if (!strcmp(what, "init_diag"))
               e[j] = nir_bcsel(&c->b, nir_ieq(&c->b, fm, nir_iadd_imm(&c->b, fn, j)), x,
                                nir_imm_zero(&c->b, 1, x->bit_size));
         }
         air_set(c, &in->base, air_val_ssa(c, nir_vec(&c->b, e, 2)));
         return true;
      }
      bool store = !strcmp(what, "store");
      if (!store && strcmp(what, "load"))
         return air_error(c, "intrinsic: air.%s", n->part[0]);
      unsigned at = store ? 2 : 1;
      if (in->num_operands != at + 4)
         return air_error(c, "intrinsic: air.%s of %u operands", n->part[0], in->num_operands - 1);
      const struct air_type *mt = store ? in->operands[1]->type : in->base.type;
      if (!air_is_simd_matrix(mt))
         return air_error(c, "intrinsic: air.%s of a value that is not a matrix", n->part[0]);
      struct air_val *pv = air_get(c, in->operands[at]);
      if (!pv)
         return false;
      if (pv->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: air.%s through a pointer chosen at run time", n->part[0]);
      nir_def *stride = air_get_ssa(c, in->operands[at + 2]);
      nir_def *origin = air_get_ssa(c, in->operands[at + 3]);
      nir_def *value = store ? air_get_ssa(c, in->operands[1]) : NULL;
      if (!stride || !origin || (store && !value))
         return false;
      stride = nir_u2u32(&c->b, stride);
      origin = nir_u2u32(&c->b, origin);
      const struct air_type *et = mt->elem;
      unsigned bytes = air_nir_bit_size(c, et) / 8;
      nir_def *row = nir_imul(&c->b, nir_iadd(&c->b, nir_channel(&c->b, origin, 1), fm),
                              nir_channel(&c->b, stride, 1));
      nir_def *got[2];
      for (unsigned j = 0; j < 2; j++) {
         nir_def *col = nir_imul(&c->b, nir_iadd(&c->b, nir_channel(&c->b, origin, 0),
                                                 nir_iadd_imm(&c->b, fn, j)),
                                 nir_channel(&c->b, stride, 0));
         struct air_ptr ep = air_ptr_add(c, pv->ptr,
                                         nir_imul_imm(&c->b, nir_iadd(&c->b, row, col), bytes));
         if (store) {
            if (!air_store_ptr(c, ep, nir_channel(&c->b, value, j), bytes))
               return false;
         } else if (!(got[j] = air_load_ptr(c, ep, et, bytes))) {
            return false;
         }
      }
      if (!store)
         air_set(c, &in->base, air_val_ssa(c, nir_vec(&c->b, got, 2)));
      return true;
   }

   /* D = A B + C for 8x8 simdgroup matrices (MLX's gemms and attention). A thread holds two
    * elements of each (air_is_simd_matrix): with l its lane in a group of 32, row
    * fm = 4 l4 + 2 l2 + l1 and columns fn, fn + 1 for fn = 4 l3 + 2 l0 -- Apple's layout, the
    * one MLX's thread_elements() code computes the same way. Row fm of A and column fn + j of
    * B come from the lanes holding them, a shuffle for each of A's eight and two for each of
    * B's eight pairs; the group of 32 is the one this lane is in, of the hardware's 64 or 128.
    * Done in 32-bit float, and given back in the type D is. */
   if (!strcmp(op, "simdgroup_matrix_8x8_multiply_accumulate")) {
      if (in->num_operands != 4 || !air_is_simd_matrix(in->base.type))
         return air_error(c, "intrinsic: air.%s of %u operands", n->part[0],
                          in->num_operands - 1);
      nir_def *m[3];
      for (unsigned k = 0; k < 3; k++) {
         const struct air_type *t = in->operands[k + 1]->type;
         if (!air_is_simd_matrix(t) || !air_type_is_float(t->elem))
            return air_error(c, "intrinsic: air.%s of a matrix that is not of floats",
                             n->part[0]);
         m[k] = air_get_ssa(c, in->operands[k + 1]);
         if (!m[k])
            return false;
         m[k] = nir_f2f32(&c->b, m[k]);
      }
      const struct air_type *rt = in->base.type;
      if (!air_type_is_float(rt->elem))
         return air_error(c, "intrinsic: air.%s to a matrix that is not of floats", n->part[0]);
      nir_def *self = nir_load_subgroup_invocation(&c->b);
      nir_def *a_lane = nir_iand_imm(&c->b, self, ~0x9u);  /* the group, l4 l2 l1: row fm */
      nir_def *b_lane = nir_iand_imm(&c->b, self, ~0x16u); /* the group, l3 l0: column fn */
      nir_def *acc[2] = { nir_channel(&c->b, m[2], 0), nir_channel(&c->b, m[2], 1) };
      for (unsigned k = 0; k < 8; k++) {
         /* A(fm, k) is element k & 1 of the lane at row fm, columns k & ~1 */
         nir_def *from_a = nir_ior_imm(&c->b, a_lane, ((k >> 2) << 3) | ((k >> 1) & 1));
         nir_def *av = nir_shuffle(&c->b, nir_channel(&c->b, m[0], k & 1), from_a);
         /* B(k, fn + j) is element j of the lane at row k, columns fn */
         nir_def *from_b = nir_ior_imm(&c->b, b_lane,
                                       ((k >> 2) << 4) | (((k >> 1) & 1) << 2) | ((k & 1) << 1));
         for (unsigned j = 0; j < 2; j++)
            acc[j] = nir_fadd(&c->b, acc[j],
                              nir_fmul(&c->b, av,
                                       nir_shuffle(&c->b, nir_channel(&c->b, m[1], j), from_b)));
      }
      air_set(c, &in->base,
              air_val_ssa(c, nir_f2fN(&c->b, nir_vec(&c->b, acc, 2), air_nir_bit_size(c, rt))));
      return true;
   }

   if (!strcmp(op, "simd_is_helper_thread")) {
      /* Exact, and width-independent: a helper thread is one whose results
       * are discarded, which is a property of the thread and not of a group.
       * ir3 runs nir_lower_is_helper_invocation itself, and this is the other
       * half of the discard above -- a demoted lane must answer yes here.
       */
      if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
         return air_error(c, "intrinsic: air.simd_is_helper_thread in a %s shader "
                             "(there are no helper threads outside the fragment "
                             "stage)", mesa_shader_stage_name(c->nir->info.stage));
      air_set(c, &in->base, air_val_ssa(c, nir_is_helper_invocation(&c->b, 1)));
      return true;
   }

   /* simd_is_first(): whether this is the first active lane of the group, which is what NIR's
    * elect answers (MetalFX's analyzeHistogram and reproject_warp) */
   if (!strcmp(op, "simd_is_first")) {
      air_set(c, &in->base, air_val_ssa(c, nir_elect(&c->b, 1)));
      return true;
   }

   if (!strcmp(op, "simd_ballot"))
      return air_error(c, "intrinsic: air.%s -- a 64-bit mask with one bit per lane "
                          "of the SIMD group, and ir3 runs this hardware's groups at "
                          "64 or 128 lanes (threadsize_base 64, doubled per variant), "
                          "so the mask this would produce cannot be known to hold "
                          "every lane; a truncated one makes popcount answer a "
                          "smaller number rather than an obviously wrong one",
                       n->part[0]);

   nir_def *a0 = in->num_operands > 1 ? air_get_ssa(c, in->operands[1]) : NULL;
   if (in->num_operands > 1 && !a0)
      return false;

   if (!strcmp(op, "simd_all") || !strcmp(op, "simd_any")) {
      bool all = !strcmp(op, "simd_all");
      air_note(c, "air.%s is taken over the SIMD group this implementation has, "
                  "which is not Metal's 32 lanes: ir3 runs 64 or 128 of them on this "
                  "hardware. air.get_simd_width answers with the same number, so a "
                  "shader that reads it is right and one with 32 built into it is not",
               n->part[0]);
      air_set(c, &in->base,
              air_val_ssa(c, all ? nir_vote_all(&c->b, 1, a0)
                                 : nir_vote_any(&c->b, 1, a0)));
      return true;
   }

   if (!strcmp(op, "simd_broadcast_first")) {
      air_note(c, "air.%s reads the first active lane of the SIMD group this "
                  "implementation has, which is not Metal's 32 lanes; the value is "
                  "the same one Metal would give wherever it is uniform across the "
                  "group, and another lane's where it is not", n->part[0]);
      air_set(c, &in->base, air_val_ssa(c, nir_read_first_invocation(&c->b, a0)));
      return true;
   }

   if (!strcmp(op, "simd_shuffle")) {
      nir_def *lane = air_get_ssa(c, in->operands[2]);
      if (!lane)
         return false;
      air_note(c, "air.%s takes its lane number in the SIMD group this "
                  "implementation has, which is not Metal's 32 lanes", n->part[0]);
      air_set(c, &in->base,
              air_val_ssa(c, nir_shuffle(&c->b, a0, nir_u2u32(&c->b, lane))));
      return true;
   }

   if (!strcmp(op, "quad_shuffle")) {
      /* quad_shuffle(data, lane): the value of lane `lane` of this thread's quad -- the quad
       * being four consecutive lanes of the SIMD group, as the quad sysvals take it -- which is
       * a shuffle to (own lane & ~3) | (lane & 3) */
      nir_def *lane = air_get_ssa(c, in->operands[2]);
      if (!lane)
         return false;
      nir_def *self = nir_load_subgroup_invocation(&c->b);
      nir_def *to = nir_ior(&c->b, nir_iand_imm(&c->b, self, ~3u),
                            nir_iand_imm(&c->b, nir_u2u32(&c->b, lane), 3));
      air_set(c, &in->base, air_val_ssa(c, nir_shuffle(&c->b, a0, to)));
      return true;
   }

   /* quad_active_threads_mask(): a bit for each lane of the thread's quad that is a real fragment and
    * not a helper, bit n for lane n -- each lane's own bit gathered by the quad's three swaps
    * (RealityKit's resolve kernels) */
   if (!strcmp(op, "quad_active_threads_mask")) {
      nir_def *self = nir_load_subgroup_invocation(&c->b);
      nir_def *bit = nir_bcsel(&c->b, nir_is_helper_invocation(&c->b, 1), nir_imm_int(&c->b, 0),
                               nir_ishl(&c->b, nir_imm_int(&c->b, 1), nir_iand_imm(&c->b, self, 3)));
      nir_def *m = nir_ior(&c->b, nir_ior(&c->b, bit, nir_quad_swap_horizontal(&c->b, bit)),
                           nir_ior(&c->b, nir_quad_swap_vertical(&c->b, bit),
                                   nir_quad_swap_diagonal(&c->b, bit)));
      air_set(c, &in->base, air_val_ssa(c, nir_u2uN(&c->b, m, air_nir_bit_size(c, in->base.type))));
      return true;
   }

   /* quad_all(b) / quad_any(b): the vote over the four lanes of the thread's quad (RealityKit's
    * unwarpVideo), by NIR's three swaps of the quad, as a word so that no 1-bit value is swapped */
   if ((!strcmp(op, "quad_all") || !strcmp(op, "quad_any")) && a0) {
      bool all = !strcmp(op, "quad_all");
      nir_def *w = nir_b2i32(&c->b, a0->bit_size == 1 ? a0 : nir_ine_imm(&c->b, a0, 0));
      nir_def *h = nir_quad_swap_horizontal(&c->b, w), *v = nir_quad_swap_vertical(&c->b, w);
      nir_def *d = nir_quad_swap_diagonal(&c->b, w);
      nir_def *r = all ? nir_iand(&c->b, nir_iand(&c->b, w, h), nir_iand(&c->b, v, d))
                       : nir_ior(&c->b, nir_ior(&c->b, w, h), nir_ior(&c->b, v, d));
      air_set(c, &in->base, air_val_ssa(c, nir_ine_imm(&c->b, r, 0)));
      return true;
   }

   if (!strcmp(op, "quad_shuffle_xor")) {
      /* quad_shuffle_xor(data, mask) hands each lane the value of the lane
       * whose number is its own exclusive-ored with the mask. Within a quad
       * that is one of three fixed swaps, which is exactly the three NIR has:
       * 1 swaps the two columns, 2 the two rows, 3 the diagonal. A mask of 0
       * is the identity.
       *
       * A mask that is not a constant, or is 4 or more, is refused rather
       * than translated: the first would need a selection over the three
       * swaps, and the second leaves the quad, where Metal's own definition
       * stops. Every call in Resident Evil 4's three modules passes a
       * constant.
       */
      nir_def *mask = air_get_ssa(c, in->operands[2]);
      if (!mask)
         return false;
      if (!nir_src_is_const(nir_src_for_ssa(mask)))
         return air_error(c, "intrinsic: air.%s with a mask that is not a constant",
                          n->part[0]);
      uint64_t m = nir_src_as_uint(nir_src_for_ssa(mask));
      switch (m) {
      case 0: air_set(c, &in->base, air_val_ssa(c, a0)); return true;
      case 1: air_set(c, &in->base,
                      air_val_ssa(c, nir_quad_swap_horizontal(&c->b, a0)));
              return true;
      case 2: air_set(c, &in->base,
                      air_val_ssa(c, nir_quad_swap_vertical(&c->b, a0)));
              return true;
      case 3: air_set(c, &in->base,
                      air_val_ssa(c, nir_quad_swap_diagonal(&c->b, a0)));
              return true;
      default:
         return air_error(c, "intrinsic: air.%s with mask %u, which leaves the quad",
                          n->part[0], (unsigned)m);
      }
   }

   /* The shifts and broadcasts within the SIMD group and within a quad, and the prefix sums.
    * Metal's lane that would read past the group (or the quad) keeps its own value; built here as
    * a shuffle to a computed lane, which gives that rule explicitly. The group is ir3's (64 or
    * 128 lanes), not Metal's 32, as for the votes above. MPSImage uses all of these. */
   if (!strcmp(op, "simd_shuffle_down") || !strcmp(op, "simd_shuffle_up") ||
       !strcmp(op, "quad_shuffle_down") || !strcmp(op, "quad_shuffle_up") ||
       !strcmp(op, "simd_shuffle_rotate_down") || !strcmp(op, "quad_shuffle_rotate_down")) {
      nir_def *d = air_get_ssa(c, in->operands[2]);
      if (!d)
         return false;
      d = nir_u2u32(&c->b, d);
      bool quad = op[0] == 'q', up = strstr(op, "_up") != NULL, rot = strstr(op, "rotate") != NULL;
      nir_def *inv = nir_load_subgroup_invocation(&c->b);
      nir_def *size = quad ? nir_imm_int(&c->b, 4) : nir_load_subgroup_size(&c->b);
      nir_def *base = quad ? nir_iand_imm(&c->b, inv, ~3u) : nir_imm_int(&c->b, 0);
      nir_def *pos = nir_isub(&c->b, inv, base);
      nir_def *to = up ? nir_isub(&c->b, pos, d) : nir_iadd(&c->b, pos, d);
      nir_def *lane;
      if (rot) {
         lane = nir_iadd(&c->b, base, nir_umod(&c->b, to, size));
      } else {
         nir_def *inside = up ? nir_uge(&c->b, pos, d) : nir_ult(&c->b, to, size);
         lane = nir_bcsel(&c->b, inside, nir_iadd(&c->b, base, to), inv);
      }
      air_set(c, &in->base, air_val_ssa(c, nir_shuffle(&c->b, a0, lane)));
      return true;
   }
   /* simd_shuffle_and_fill_down(data, filling, delta[, modulo]) and _up: the shift above within
    * groups of `modulo` lanes (the SIMD group without it), and a lane that would read past its
    * group's end (start) reads `filling` of the lane as far into the next (previous) group instead
    * -- the two-register window a separable filter slides along a row. DeepVideoProcessingCore's
    * gaussian3x3_filter_SIMD, with modulo 4 and the group size. */
   if (!strcmp(op, "simd_shuffle_and_fill_down") || !strcmp(op, "simd_shuffle_and_fill_up")) {
      nir_def *fill = air_get_ssa(c, in->operands[2]);
      nir_def *d = in->num_operands > 3 ? air_get_ssa(c, in->operands[3]) : NULL;
      nir_def *m = in->num_operands > 4 ? air_get_ssa(c, in->operands[4]) : NULL;
      if (!fill || !d || (in->num_operands > 4 && !m))
         return false;
      bool up = strstr(op, "_up") != NULL;
      d = nir_u2u32(&c->b, d);
      nir_def *size = m ? nir_u2u32(&c->b, m) : nir_load_subgroup_size(&c->b);
      nir_def *inv = nir_load_subgroup_invocation(&c->b);
      nir_def *pos = nir_umod(&c->b, inv, size);
      nir_def *base = nir_isub(&c->b, inv, pos);
      nir_def *inside = up ? nir_uge(&c->b, pos, d) : nir_ult(&c->b, nir_iadd(&c->b, pos, d), size);
      /* (pos - d + size and pos + d - size are the filling lane's place, in range exactly where
       * the data lane's is not: d is less than the group, as Metal requires) */
      nir_def *to = up ? nir_isub(&c->b, pos, d) : nir_iadd(&c->b, pos, d);
      nir_def *from = up ? nir_iadd(&c->b, to, size) : nir_isub(&c->b, to, size);
      nir_def *dv = nir_shuffle(&c->b, a0, nir_iadd(&c->b, base, nir_bcsel(&c->b, inside, to, pos)));
      nir_def *fv = nir_shuffle(&c->b, fill, nir_iadd(&c->b, base, nir_bcsel(&c->b, inside, pos, from)));
      air_set(c, &in->base, air_val_ssa(c, nir_bcsel(&c->b, inside, dv, fv)));
      return true;
   }
   /* simd_shuffle_xor(data, mask): the lane whose number is this one's exclusive-ored with the mask */
   if (!strcmp(op, "simd_shuffle_xor")) {
      nir_def *mask = air_get_ssa(c, in->operands[2]);
      if (!mask)
         return false;
      nir_def *lane = nir_ixor(&c->b, nir_load_subgroup_invocation(&c->b), nir_u2u32(&c->b, mask));
      air_set(c, &in->base, air_val_ssa(c, nir_shuffle(&c->b, a0, lane)));
      return true;
   }
   if (!strcmp(op, "simd_broadcast")) {
      nir_def *l = air_get_ssa(c, in->operands[2]);
      if (!l)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_shuffle(&c->b, a0, nir_u2u32(&c->b, l))));
      return true;
   }
   if (!strcmp(op, "quad_broadcast")) {
      nir_def *l = air_get_ssa(c, in->operands[2]);
      if (!l)
         return false;
      nir_def *inv = nir_load_subgroup_invocation(&c->b);
      nir_def *lane = nir_ior(&c->b, nir_iand_imm(&c->b, inv, ~3u),
                              nir_iand_imm(&c->b, nir_u2u32(&c->b, l), 3));
      air_set(c, &in->base, air_val_ssa(c, nir_shuffle(&c->b, a0, lane)));
      return true;
   }
   if (!strcmp(op, "simd_prefix_exclusive_sum") || !strcmp(op, "simd_prefix_inclusive_sum") ||
       !strcmp(op, "simd_prefix_exclusive_product") || !strcmp(op, "simd_prefix_inclusive_product")) {
      bool fl = air_type_is_float(air_type_scalar(in->base.type));
      bool sum = strstr(op, "_sum") != NULL;
      nir_op r = sum ? (fl ? nir_op_fadd : nir_op_iadd) : (fl ? nir_op_fmul : nir_op_imul);
      nir_def *v = strstr(op, "exclusive") ? nir_exclusive_scan(&c->b, a0, .reduction_op = r)
                                           : nir_inclusive_scan(&c->b, a0, .reduction_op = r);
      air_set(c, &in->base, air_val_ssa(c, v));
      return true;
   }

   /* air.get_sample_position(index, 0): the position of one sample within the
    * pixel, which the hardware's sample pattern decides.
    *
    * REFUSED BY NAME, and the reason is half a pixel. NIR has the operation
    * and the Adreno has the instruction -- nir_load_sample_pos_from_id, which
    * ir3 emits as RGETPOS -- but that intrinsic's convention is an OFFSET
    * from the pixel's centre, in [-0.5, 0.5]: ir3's own lowering of
    * gl_SamplePosition adds (0.5, 0.5) to it and cites the dEQP case that
    * fixes the difference (ir3_nir_lower_load_barycentric_at_sample.c).
    * Metal's convention is not stated by anything on this machine -- not by
    * the metal_graphics header on the device, which only names the function
    * and its MSL section, and not by any string in the compiler libraries --
    * and the two readings differ by exactly half a pixel, which is the kind
    * of wrongness that looks like a slightly soft image rather than like a
    * bug. 4 of Resident Evil 4's 7 879 modules call it, and the way to settle
    * it is a measurement on the phone (draw one multisampled pixel, read the
    * positions back) rather than a choice made here.
    */
   if (!strcmp(op, "get_sample_position"))
      return air_error(c, "intrinsic: air.%s -- NIR's sample position is an offset "
                          "from the pixel centre and Metal's convention is not "
                          "attested anywhere on this machine; the two differ by half "
                          "a pixel, so this is left to a measurement", n->part[0]);

   *handled = false;
   return true;
}

/* -- llvm.* ------------------------------------------------------------- */

/* A memcpy whose length is a run-time value (the BVH builder's fastGPUMemcpyKernel and its
 * copies of primitive data): a loop of words while four bytes are left, then one of bytes,
 * each built of blocks (air_loop_head) with its byte offset in a register. */
static bool
memcpy_runtime(struct air_ctx *c, struct air_ptr dst, struct air_ptr src,
               const struct air_value *length)
{
   nir_def *len = air_get_ssa(c, length);
   if (!len)
      return false;
   len = nir_u2u32(&c->b, len);
   static const struct air_type u32 = { .kind = AIR_TYPE_INT, .bits = 32 };
   static const struct air_type u8 = { .kind = AIR_TYPE_INT, .bits = 8 };
   nir_builder at = nir_builder_at(nir_before_impl(c->b.impl));
   nir_def *reg = nir_decl_reg(&at, 1, 32, 0);
   nir_store_reg(&c->b, nir_imm_int(&c->b, 0), reg);
   static const unsigned steps[] = { 4, 1 };
   for (unsigned k = 0; k < 2; k++) {
      unsigned step = steps[k];
      struct air_loop lp;
      air_loop_head(c, &lp);
      nir_def *off = nir_load_reg(&c->b, reg);
      air_loop_test(c, &lp, nir_uge(&c->b, len, nir_iadd_imm(&c->b, off, step)));
      off = nir_load_reg(&c->b, reg);
      struct air_ptr s = air_ptr_add(c, src, off), d = air_ptr_add(c, dst, off);
      nir_def *v = air_load_ptr(c, s, step == 4 ? &u32 : &u8, step);
      if (!v || !air_store_ptr(c, d, v, step))
         return false;
      nir_store_reg(&c->b, nir_iadd_imm(&c->b, off, step), reg);
      air_loop_end(c, &lp);
   }
   return true;
}

static bool
llvm_op(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   if (!strcmp(name, "llvm.assume"))
      return true;      /* a promise to the optimiser, nothing to compute */
   /* Apple's own hint, in a spin loop, that the lane may give way to others (MPSCore's
    * single_pass_scan waiting on its neighbour's partial sum): nothing to compute either */
   if (!strncmp(name, "llvm.agx", 8) && strstr(name, ".yield"))
      return true;
   if (!strncmp(name, "llvm.bswap.", 11)) {
      /* the bytes of each component in the other order (VideoToolbox's big-endian formats) */
      nir_def *x = air_get_ssa(c, in->operands[1]);
      if (!x)
         return false;
      nir_def *r;
      if (x->bit_size == 16) {
         r = nir_ior(&c->b, nir_ushr_imm(&c->b, x, 8), nir_ishl_imm(&c->b, x, 8));
      } else if (x->bit_size == 32) {
         r = nir_ior(&c->b,
                     nir_ior(&c->b, nir_ushr_imm(&c->b, x, 24), nir_ishl_imm(&c->b, x, 24)),
                     nir_ior(&c->b, nir_iand_imm(&c->b, nir_ushr_imm(&c->b, x, 8), 0xff00),
                             nir_iand_imm(&c->b, nir_ishl_imm(&c->b, x, 8), 0xff0000)));
      } else {
         return air_error(c, "intrinsic: %s on %u bits", name, x->bit_size);
      }
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   if (!strncmp(name, "llvm.lifetime.", 14))
      return true; /* scope markers: nothing to emit */
   if (!strncmp(name, "llvm.memcpy.", 12)) {
      /* a copy between two of our pointers, of a constant length */
      struct air_val *dst = air_get(c, in->operands[1]);
      struct air_val *src = air_get(c, in->operands[2]);
      if (!dst || !src)
         return false;
      if (dst->kind != AIR_VAL_PTR || src->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: memcpy that is not between pointers");
      uint64_t bytes;
      if (!air_const_uint(in->operands[3], &bytes))
         return memcpy_runtime(c, dst->ptr, src->ptr, in->operands[3]);
      /* in 4-byte pieces, which is what most memcpys are a multiple of (they copy float vectors
       * out of constant buffers), and the bytes after the last whole word one at a time: a
       * struct of 30 bytes copied (NRFV1, the camera's noise reduction) */
      static const struct air_type u32 = { .kind = AIR_TYPE_INT, .bits = 32 };
      static const struct air_type u8 = { .kind = AIR_TYPE_INT, .bits = 8 };
      uint64_t off = 0;
      for (; off + 4 <= bytes; off += 4) {
         struct air_ptr s = air_ptr_add(c, src->ptr, nir_imm_int(&c->b, off));
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         nir_def *v = air_load_ptr(c, s, &u32, 4);
         if (!v || !air_store_ptr(c, d, v, 4))
            return false;
      }
      for (; off < bytes; off++) {
         struct air_ptr s = air_ptr_add(c, src->ptr, nir_imm_int(&c->b, off));
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         nir_def *v = air_load_ptr(c, s, &u8, 1);
         if (!v || !air_store_ptr(c, d, v, 1))
            return false;
      }
      return true;
   }
   if (!strncmp(name, "llvm.memset.", 12)) {
      struct air_val *dst = air_get(c, in->operands[1]);
      nir_def *byte = air_get_ssa(c, in->operands[2]);
      if (!dst || !byte)
         return false;
      if (dst->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: memset of something that is not a pointer");
      uint64_t bytes;
      if (!air_const_uint(in->operands[3], &bytes))
         return air_error(c, "intrinsic: memset of a run-time length");
      if (!nir_src_is_const(nir_src_for_ssa(byte)))
         return air_error(c, "intrinsic: memset of a run-time byte");
      uint32_t b = nir_src_as_uint(nir_src_for_ssa(byte)) & 0xff;
      uint32_t word = b | b << 8 | b << 16 | b << 24;
      /* words while they last, then the bytes after them -- a struct of 30 bytes cleared
       * (Portrait's and FRC's linePredictOutput kernels, NRFV1) */
      uint64_t off = 0;
      for (; off + 4 <= bytes; off += 4) {
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         if (!air_store_ptr(c, d, nir_imm_int(&c->b, word), 4))
            return false;
      }
      for (; off < bytes; off++) {
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         if (!air_store_ptr(c, d, nir_imm_intN_t(&c->b, b, 8), 1))
            return false;
      }
      return true;
   }
   /* llvm.fmuladd(a, b, c): a multiply-add that MAY be fused.
    *
    * Translated as a fused one -- nir_ffma, a single rounding -- and the
    * licence for that is the intrinsic's own definition rather than the
    * module's fast-math flag. LLVM's Language Reference says of llvm.fmuladd
    * that the expression "is equivalent to a * b + c, except that it is
    * unspecified whether rounding will be performed between the
    * multiplication and addition steps", and that a caller who needs the
    * fusion guaranteed must use llvm.fma instead. A producer that emitted
    * llvm.fmuladd rather than `fmul` + `fadd` therefore accepted both answers
    * when it chose the name, and the choice between them is the back end's.
    *
    * SO THE ABSENCE OF air.compile.fast_math_enable CHANGES NOTHING HERE, and
    * that is worth saying because the measurement invites the opposite
    * reading: 383 of a 394-module sample of Resident Evil 4 carry the flag, so
    * contraction is permitted under the stricter reading too -- but the eleven
    * that do not are not a different case for THIS intrinsic. A module that
    * wanted two roundings would carry two instructions.
    *
    * Which answer the Adreno gives is then ir3's: nir_ffma is one rounding,
    * and a backend that has no fused multiply-add would lower it. softpipe
    * does not have one -- TGSI's own specification lets MAD round the product
    * first and its interpreter does -- which is why tools/air/intrincheck.py
    * sets air.fma aside on softpipe and leaves it to the phone. The same
    * applies to every llvm.fmuladd this translates.
    */
   if (!strncmp(name, "llvm.fmuladd.", 13)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      nir_def *d = air_get_ssa(c, in->operands[3]);
      if (!a || !b || !d)
         return false;
      air_set(c, &in->base, air_val_ssa(c, air_fma(&c->b, a, b, d)));
      return true;
   }
   if (!strncmp(name, "llvm.fabs.", 10)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      if (!a)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_fabs(&c->b, a)));
      return true;
   }
   /* llvm.{s,u}{min,max}.<type>: what LLVM folds a compare-and-select into
    * (CoreImage's convert kernels, on i16)
    */
   if (!strncmp(name, "llvm.smin.", 10) || !strncmp(name, "llvm.smax.", 10) ||
       !strncmp(name, "llvm.umin.", 10) || !strncmp(name, "llvm.umax.", 10)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      if (!a || !b)
         return false;
      bool is_max = name[7] == 'a', is_signed = name[5] == 's';
      nir_def *r = is_signed ? (is_max ? nir_imax(&c->b, a, b) : nir_imin(&c->b, a, b))
                             : (is_max ? nir_umax(&c->b, a, b) : nir_umin(&c->b, a, b));
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   /* llvm.{min,max}num.<type>: what LLVM folds fmin()/fmax() of half and float into when the
    * call is fast-math (ARKit's stencil_bilinear_resample and RawCamera's focus-pixel
    * correction, on f16; Darwin 27). A NaN operand gives the other operand there; NIR's
    * fmin/fmax leave NaN to the backend, and ir3's min.f/max.f return the non-NaN one. */
   if (!strncmp(name, "llvm.minnum.", 12) || !strncmp(name, "llvm.maxnum.", 12)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      if (!a || !b)
         return false;
      air_set(c, &in->base, air_val_ssa(c, name[6] == 'a' ? nir_fmax(&c->b, a, b)
                                                         : nir_fmin(&c->b, a, b)));
      return true;
   }
   /* llvm.abs.<type>(x, i1 int_min_is_poison): abs() of an integer (RawCamera, i16). The
    * flag only says what INT_MIN may give; iabs gives INT_MIN for it, which is LLVM's answer
    * when the flag is false. */
   if (!strncmp(name, "llvm.abs.", 9)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      if (!a)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_iabs(&c->b, a)));
      return true;
   }
   /* llvm.bitreverse.i32: reverse_bits() (ARKit's blur_cubemap: a radical inverse, the
    * Hammersley sequence). NIR reverses 32 bits; a narrower type is reversed in 32 and
    * shifted down, a wider one is not done. */
   if (!strncmp(name, "llvm.bitreverse.", 16)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      if (!a)
         return false;
      unsigned bits = a->bit_size;
      if (bits > 32)
         return air_error(c, "intrinsic: %s (wider than NIR's 32-bit bitfield_reverse)", name);
      nir_def *r = nir_bitfield_reverse(&c->b, bits == 32 ? a : nir_u2u32(&c->b, a));
      if (bits < 32)
         r = nir_u2uN(&c->b, nir_ushr_imm(&c->b, r, 32 - bits), bits);
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   /* The saturating ones, LLVM's spelling of add_sat / sub_sat (MetalFX, MTLBVHBuilder,
    * MPSNeuralNetwork): NIR has all four. */
   if (!strncmp(name, "llvm.uadd.sat.", 14) || !strncmp(name, "llvm.usub.sat.", 14) ||
       !strncmp(name, "llvm.sadd.sat.", 14) || !strncmp(name, "llvm.ssub.sat.", 14)) {
      nir_def *a = air_get_ssa(c, in->operands[1]), *b = air_get_ssa(c, in->operands[2]);
      if (!a || !b)
         return false;
      bool add = name[6] == 'a', sgn = name[5] == 's';
      nir_def *r = add ? (sgn ? nir_iadd_sat(&c->b, a, b) : nir_uadd_sat(&c->b, a, b))
                       : (sgn ? nir_isub_sat(&c->b, a, b) : nir_usub_sat(&c->b, a, b));
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   /* Bit counts, as air.popcount / clz / ctz are done (at the argument's width; NIR answers
    * 32 bits). LLVM's ctlz and cttz take a second operand saying zero is undefined, which
    * only licenses an answer, so the defined one -- the width -- is given whatever it says. */
   if (!strncmp(name, "llvm.ctpop.", 11) || !strncmp(name, "llvm.ctlz.", 10) ||
       !strncmp(name, "llvm.cttz.", 10)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      if (!a)
         return false;
      unsigned w = a->bit_size;
      nir_def *r;
      if (name[7] == 'p') {
         r = nir_bit_count(&c->b, a);
      } else if (name[7] == 'l') {
         nir_def *msb = nir_ufind_msb(&c->b, w < 32 ? nir_u2u32(&c->b, a) : a);
         r = nir_isub(&c->b, nir_imm_int(&c->b, (int)w - 1), msb);
      } else {
         r = nir_bcsel(&c->b, nir_ieq_imm(&c->b, a, 0), nir_imm_int(&c->b, (int)w),
                       nir_find_lsb(&c->b, a));
      }
      air_set(c, &in->base, air_val_ssa(c, nir_u2uN(&c->b, r, w)));
      return true;
   }
   /* Funnel shifts: fshl(a, b, s) is the high half of (a:b) << (s mod w), fshr the low half of
    * (a:b) >> (s mod w); with a and b the same value they are rotates, which is what Morpheus's
    * and MLX's rbits use them for. In two shifts and an or, the zero shift taken apart because
    * a shift by the full width is not a shift in NIR. */
   if (!strncmp(name, "llvm.fshl.", 10) || !strncmp(name, "llvm.fshr.", 10)) {
      nir_def *a = air_get_ssa(c, in->operands[1]), *b = air_get_ssa(c, in->operands[2]);
      nir_def *sh = air_get_ssa(c, in->operands[3]);
      if (!a || !b || !sh)
         return false;
      unsigned w = a->bit_size;
      nir_def *k = nir_umod_imm(&c->b, nir_u2u32(&c->b, sh), w);
      nir_def *kk = nir_isub(&c->b, nir_imm_int(&c->b, (int)w), k);
      bool left = name[8] == 'l';
      nir_def *r = left ? nir_ior(&c->b, nir_ishl(&c->b, a, k), nir_ushr(&c->b, b, kk))
                        : nir_ior(&c->b, nir_ushr(&c->b, b, k), nir_ishl(&c->b, a, kk));
      r = nir_bcsel(&c->b, nir_ieq_imm(&c->b, k, 0), left ? a : b, r);
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   if (!strcmp(name, "llvm.trap"))
      return air_error(c, "intrinsic: llvm.trap");
   return air_error(c, "intrinsic: %s", name);
}

/* -- textures ------------------------------------------------------------
 *
 * The largest family and the one with the long tail. Its vocabulary is
 * Apple's, read off AGXCompilerCore the same way the arithmetic was
 * (docs/air.md section 3.2): the names there end in an underscore --
 * air.sample_texture_, air.gather_compare_depth_, air.get_num_mip_levels_texture_
 * -- because the dimensionality is appended to them. That is why the earlier
 * survey counted "about 117 get_*_texture_* variants": it is some twenty
 * operations crossed with nine dimensionalities, not a hundred separate
 * things.
 *
 * The signatures are measured from the corpus, not guessed:
 *
 *   sample_<kind>_<dim> (tex, samp, [plane,] coord, [i32 array,]
 *                        [i1 has_offset, offset,]
 *                        i1 lod_is_level, float lod_or_bias, float min_lod,
 *                        i32 flags)                  -> { <4 x T>, i8 }
 *   sample_<kind>_<dim>_grad
 *                       (tex, samp, [plane,] coord, [i32 array,]
 *                        dPdx, dPdy, float min_lod,
 *                        [i1 has_offset, offset,] i32 flags)
 *   gather_<kind>_<dim> (tex, samp, coord, [i32 array,]
 *                        [i1 has_offset, offset,]
 *                        i32 component, i32 flags)   -> { <4 x T>, i8 }
 *   read_<kind>_<dim>   (tex, [plane,] coord, [i32 array,]
 *                        i32 lod_or_sample, i32 flags)
 *   write_<kind>_<dim>  (tex, coord, [i32 array,] value, i32 lod, i32 flags)
 *   get_<what>_<kind>_<dim> (tex[, i32 lod])         -> i32
 *
 * Three things in those brackets, and none of them is a guess. They are read
 * off Apple's own metal_stdlib on this device --
 * GPUCompiler.framework/.../include/metal/metal_texture, where every
 * `texture2d_array<T>::sample` and its fifty neighbours are one-line
 * forwarders to the builtin, so the header states the argument ORDER for all
 * nine dimensionalities and six operations -- and then confirmed against the
 * LLVM declaration in the modules that call them. The two agree everywhere:
 *
 *   AN ARRAY INDEX IS ITS OWN ARGUMENT, straight after the coordinate, in
 *   every one of sample, gather, read and write. It is not inside the
 *   coordinate vector, which is what this file used to require of it:
 *   air.sample_texture_2d_array takes a <2 x float> and an i32, and the
 *   refusal it used to earn said "with a 2-component coordinate, wanting 3".
 *
 *   THE GRADIENT FORM REORDERS WHAT FOLLOWS. dPdx, dPdy and the minimum level
 *   come before the offset pair, where the plain form has the offset pair
 *   first and the level last. A reader that assumed one order for both would
 *   take a gradient for an offset flag.
 *
 *   A CUBE HAS NO OFFSET PAIR AT ALL -- Metal defines no offset for cube
 *   sampling -- so air.sample_texture_cube goes from the coordinate straight
 *   to the level flag. Reading the 2d shape there is what made 621 modules
 *   stop at "argument 5 is not a constant": the float minimum level was being
 *   read as the i1 that says whether the next float is a level.
 *
 * The i1 before the level is read as "the float is a level, not a bias", so
 * (false, 0.0) is a plain sample -- which is what every one of the system
 * corpus's sample calls passes. Resident Evil 4 exercises both: 23 823
 * level-form calls against 5 961 bias-form.
 *
 * The second field of the returned struct is Metal's sparse-residency byte.
 * No module in the corpus reads it (every extractvalue of a texture result
 * takes field 0), so it is produced as zero rather than invented.
 */

struct air_tex_name {
   const char *op;        /* "sample", "gather", "read", "write", "get", "lod" */
   const char *query;     /* for "get": width, height, depth, array_size, ... */
   bool compare;          /* sample_compare / gather_compare */
   bool depth;            /* a depth texture rather than a colour one */
   enum glsl_sampler_dim dim;
   bool is_array, is_ms, is_buffer;
   bool grad;             /* the _grad form: explicit dPdx and dPdy */
   bool no_offset;        /* this dimensionality takes no offset argument */
   const char *atomic;    /* "atomic": add, sub, and, or, xor, min, max, exchange */
};

static bool
eat(const char **s, const char *prefix)
{
   size_t n = strlen(prefix);
   if (strncmp(*s, prefix, n))
      return false;
   *s += n;
   return true;
}

/* air.<op>_<kind>_<dim> -> the pieces, or false when it is not a texture name */
static bool
parse_tex_name(const char *name, struct air_tex_name *t)
{
   memset(t, 0, sizeof(*t));
   const char *s = name;

   if (eat(&s, "sample_compare_")) {
      t->op = "sample";
      t->compare = true;
   } else if (eat(&s, "sample_")) {
      t->op = "sample";
   } else if (eat(&s, "gather_compare_")) {
      t->op = "gather";
      t->compare = true;
   } else if (eat(&s, "gather_")) {
      t->op = "gather";
   } else if (eat(&s, "read_")) {
      t->op = "read";
   } else if (eat(&s, "write_")) {
      t->op = "write";
   } else if (eat(&s, "atomic_")) {
      /* air.atomic_fetch_max_explicit_texture_2d (MetalFX's motion-vector splats) */
      static const char *ops[] = { "add", "sub", "and", "or", "xor", "min", "max" };
      static char *const pre[] = { "fetch_add_explicit_", "fetch_sub_explicit_",
                                   "fetch_and_explicit_", "fetch_or_explicit_",
                                   "fetch_xor_explicit_", "fetch_min_explicit_",
                                   "fetch_max_explicit_" };
      t->op = "atomic";
      for (unsigned i = 0; i < ARRAY_SIZE(ops) && !t->atomic; i++)
         if (eat(&s, pre[i]))
            t->atomic = ops[i];
      if (!t->atomic && eat(&s, "exchange_explicit_"))
         t->atomic = "exchange";
      if (!t->atomic)
         return false;
   } else if (eat(&s, "calculate_clamped_lod_") || eat(&s, "calculate_unclamped_lod_")) {
      t->op = "lod";
   } else if (eat(&s, "get_")) {
      t->op = "get";
      static const char *queries[] = {
         "num_mip_levels_", "num_samples_", "array_size_", "width_", "height_", "depth_",
      };
      static const char *names[] = {
         "num_mip_levels", "num_samples", "array_size", "width", "height", "depth",
      };
      for (unsigned i = 0; i < ARRAY_SIZE(queries); i++) {
         if (eat(&s, queries[i])) {
            t->query = names[i];
            break;
         }
      }
      if (!t->query)
         return false;
   } else {
      return false;
   }

   if (eat(&s, "texture_buffer_1d")) {
      t->dim = GLSL_SAMPLER_DIM_BUF;
      t->is_buffer = true;
      return *s == 0;
   }
   if (eat(&s, "depth_"))
      t->depth = true;
   else if (!eat(&s, "texture_"))
      return false;

   /* The gradient form appends _grad to the DIMENSIONALITY and not to the
    * operation: Apple's builtin is __metal_sample_texture_2d_array_t_grad, so
    * the air.* name is air.sample_texture_2d_array_grad. Taking it off here
    * rather than giving the table nine more rows is what keeps one name for
    * one dimensionality -- and the exact comparison below is why 858 modules
    * used to stop at "a texture name this cannot take apart".
    */
   size_t len = strlen(s);
   if (len > 5 && !strcmp(s + len - 5, "_grad")) {
      t->grad = true;
      len -= 5;
   }

   /* the dimensionality, and whether it has an offset argument at all */
   static const struct {
      const char *suffix;
      enum glsl_sampler_dim dim;
      bool array, ms, no_offset;
   } dims[] = {
      { "cube_array", GLSL_SAMPLER_DIM_CUBE, true, false, true },
      { "2d_ms_array", GLSL_SAMPLER_DIM_MS, true, true, false },
      { "2d_array", GLSL_SAMPLER_DIM_2D, true, false, false },
      { "1d_array", GLSL_SAMPLER_DIM_1D, true, false, false },
      { "2d_ms", GLSL_SAMPLER_DIM_MS, false, true, false },
      { "cube", GLSL_SAMPLER_DIM_CUBE, false, false, true },
      { "1d", GLSL_SAMPLER_DIM_1D, false, false, false },
      { "2d", GLSL_SAMPLER_DIM_2D, false, false, false },
      { "3d", GLSL_SAMPLER_DIM_3D, false, false, false },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(dims); i++) {
      if (strlen(dims[i].suffix) == len && !strncmp(s, dims[i].suffix, len)) {
         t->dim = dims[i].dim;
         t->is_array = dims[i].array;
         t->is_ms = dims[i].ms;
         t->no_offset = dims[i].no_offset;
         return true;
      }
   }
   return false;
}

static unsigned
coord_components_of(const struct air_tex_name *t)
{
   unsigned n = t->dim == GLSL_SAMPLER_DIM_1D ? 1 :
                t->dim == GLSL_SAMPLER_DIM_3D ? 3 :
                t->dim == GLSL_SAMPLER_DIM_CUBE ? 3 :
                t->dim == GLSL_SAMPLER_DIM_BUF ? 1 : 2;
   return n + (t->is_array ? 1 : 0);
}

/* the result type of a texture operation, from the LLVM type it returns */
static nir_alu_type
tex_dest_type(struct air_ctx *c, const struct air_type *ret, const struct air_name *n)
{
   const struct air_type *t = ret;
   if (t->kind == AIR_TYPE_STRUCT && t->num_members)
      t = t->members[0];
   const struct air_type *scalar = air_type_scalar(t);
   unsigned bits = air_type_bit_size(scalar);
   if (air_type_is_float(scalar))
      return nir_type_float | bits;
   /* the signedness is in the name, not in the type: .s or .u */
   for (unsigned i = 1; i < n->num; i++) {
      if (!strcmp(n->part[i], "s"))
         return nir_type_int | bits;
      if (!strcmp(n->part[i], "u"))
         return nir_type_uint | bits;
   }
   return nir_type_uint | bits;
}

/* The pointer a texture or sampler argument carries. */
static bool
tex_handle(struct air_ctx *c, const struct air_inst *in, unsigned operand,
           enum air_ptr_class want, struct air_ptr *out)
{
   struct air_val *v = air_get(c, in->operands[operand]);
   if (!v)
      return false;
   if (v->kind != AIR_VAL_PTR || v->ptr.class != want)
      return air_error(c, "intrinsic: argument %u is not a %s", operand - 1,
                       want == AIR_PTR_TEXTURE ? "texture" : "sampler");
   *out = v->ptr;
   return true;
}

/* a constant i1/i32 argument, which these intrinsics use as flags */
static bool
tex_flag(struct air_ctx *c, const struct air_inst *in, unsigned operand, uint64_t *out)
{
   /* past the call's own operands is a signature this file has wrong, and not a crash */
   if (operand >= in->num_operands)
      return air_error(c, "intrinsic: argument %u of %u", operand - 1, in->num_operands - 1);
   if (!air_const_uint(in->operands[operand], out))
      return air_error(c, "intrinsic: argument %u is not a constant", operand - 1);
   return true;
}

/*
 * WHICH MIP LEVEL a write goes to, as a place rather than a number: a constant, or a word a constant
 * buffer holds -- through casts of it, and an element of a vector loaded from it. MercuryPoster's
 * VariableBlurrer (the lock screen's blurred wallpapers) writes each level of its chain with
 *
 *   %7 = load <2 x i16>, ptr addrspace(2) %3          ; its uniforms: [[buffer(0)]]
 *   %8 = extractelement <2 x i16> %7, i64 1
 *   call void @air.write_texture_2d.i16.v4f16(ptr addrspace(1) %1, <2 x i16> %2, <4 x half> %30, i16 %8, i32 2)
 *
 * and Gallium binds an image at one level (pipe_image_view.u.tex.level), with no level in the
 * store; so the level is taken where it is: at the bind (air_metal.c reads the word), and the
 * store writes level 0 of a view made at it.
 */
struct air_level_place {
   bool constant;
   uint64_t value;
   unsigned buffer, offset, bytes;
   bool is_float;   /* the word is a float, and the level its whole part (SiriBlur's) */
   int bias;        /* added to the word: `add i16 %word, -1` (CoreAnimation's variable blur) */
};

/* the name of the function a call instruction calls, or NULL */
static const char *
called_name(const struct air_inst *in)
{
   if (in->op != AIR_OP_CALL || !in->num_operands || in->operands[0]->kind != AIR_VALUE_FUNCTION)
      return NULL;
   return in->operands[0]->name;
}

static bool
level_place(struct air_ctx *c, const struct air_value *v, struct air_level_place *out)
{
   uint64_t k;
   if (air_const_uint(v, &k)) {
      *out = (struct air_level_place){ .constant = true, .value = k };
      return true;
   }
   unsigned elem = 0;
   bool have_elem = false, is_float = false, have_bias = false;
   int bias = 0;
   for (;;) {
      if (v->kind != AIR_VALUE_INSTRUCTION)
         return false;
      const struct air_inst *in = (const struct air_inst *)v;
      /* the word plus a constant, once, before anything else */
      if (in->op == AIR_OP_BINOP && in->sub == AIR_BINOP_ADD && !have_bias && !have_elem &&
          !is_float && in->base.type && in->base.type->kind == AIR_TYPE_INT &&
          in->base.type->bits <= 32 && air_const_uint(in->operands[1], &k)) {
         unsigned w = in->base.type->bits;
         bias = (int)(w < 32 ? (int32_t)((uint32_t)k << (32 - w)) >> (32 - w) : (int32_t)k);
         have_bias = true;
         v = in->operands[0];
         continue;
      }
      /* a float made a whole number: air.convert.u.iN.f.f32, of air.(fast_)floor.f32 or not --
       * SiriBlur's level is (ushort)floor(the float its uniforms hold) */
      const char *fn = called_name(in);
      if (fn && !is_float && !have_elem && in->num_operands == 2 &&
          (!strncmp(fn, "air.convert.u.i", 15) || !strncmp(fn, "air.convert.s.i", 15)) &&
          strstr(fn, ".f.f32")) {
         v = in->operands[1];
         if (v->kind == AIR_VALUE_INSTRUCTION) {
            const char *f2 = called_name((const struct air_inst *)v);
            if (f2 && (!strcmp(f2, "air.fast_floor.f32") || !strcmp(f2, "air.floor.f32")))
               v = ((const struct air_inst *)v)->operands[1];
         }
         is_float = true;
         /* ... of the float plus a whole number: (ushort)(f + 1.0) is (ushort)f + 1 for any f
          * a level can be (VFX's compute_bloom_down_sample_lod writes at its uniform's level plus
          * one) */
         if (!have_bias && v->kind == AIR_VALUE_INSTRUCTION) {
            const struct air_inst *add = (const struct air_inst *)v;
            const struct air_constant *kc = add->num_operands == 2
                                               ? air_value_as_constant(add->operands[1]) : NULL;
            if (add->op == AIR_OP_BINOP && add->sub == AIR_BINOP_ADD && add->base.type &&
                add->base.type->kind == AIR_TYPE_FLOAT && kc &&
                kc->ckind == AIR_CONST_FLOAT) {
               float f;
               uint32_t fb = (uint32_t)kc->u;
               memcpy(&f, &fb, 4);
               if (f == (float)(int)f && f >= 0 && f < 16) {
                  bias = (int)f;
                  have_bias = true;
                  v = add->operands[0];
               }
            }
         }
         continue;
      }
      if (in->op == AIR_OP_CAST && (in->sub == AIR_CAST_TRUNC || in->sub == AIR_CAST_ZEXT ||
                                    in->sub == AIR_CAST_SEXT) && !have_elem) {
         v = in->operands[0];
         continue;
      }
      if (in->op == AIR_OP_EXTRACTELT && !have_elem) {
         if (!air_const_uint(in->operands[1], &k))
            return false;
         elem = (unsigned)k;
         have_elem = true;
         v = in->operands[0];
         continue;
      }
      if (in->op != AIR_OP_LOAD)
         return false;
      const struct air_type *t = in->base.type;
      const struct air_type *st = t->kind == AIR_TYPE_VECTOR ? t->elem : t;
      if (!st || (have_elem != (t->kind == AIR_TYPE_VECTOR)))
         return false;
      if (is_float ? st->kind != AIR_TYPE_FLOAT
                   : st->kind != AIR_TYPE_INT || st->bits % 8 || st->bits > 32)
         return false;
      unsigned bytes = is_float ? 4 : st->bits / 8;
      struct air_val *p = air_get(c, in->operands[0]);
      if (!p || p->kind != AIR_VAL_PTR || p->ptr.class != AIR_PTR_UBO ||
          (p->ptr.offset && !nir_src_is_const(nir_src_for_ssa(p->ptr.offset))))
         return false;
      unsigned off = p->ptr.offset ? (unsigned)nir_src_as_uint(nir_src_for_ssa(p->ptr.offset)) : 0;
      *out = (struct air_level_place){
         .buffer = p->ptr.binding, .offset = off + elem * bytes, .bytes = bytes, .is_float = is_float,
         .bias = bias,
      };
      return true;
   }
}

static bool
level_place_equal(const struct air_level_place *a, const struct air_level_place *b)
{
   return a->constant == b->constant &&
          (a->constant ? a->value == b->value
                       : a->buffer == b->buffer && a->offset == b->offset && a->bytes == b->bytes &&
                            a->is_float == b->is_float && a->bias == b->bias);
}

static struct air_resource *
image_resource(struct air_ctx *c, unsigned binding)
{
   for (unsigned i = 0; c->info && i < c->info->num_resources; i++)
      if (c->info->resources[i].kind == AIR_RESOURCE_IMAGE && c->info->resources[i].index == binding)
         return &c->info->resources[i];
   return NULL;
}

/* The image is bound at this level -- as a write or a size query has already said, or from now on.
 * One view, one level: a shader naming two is refused. */
static bool image_at_level(struct air_ctx *c, struct air_resource *r, const struct air_level_place *lp,
                           const char *what, unsigned *binding);

/* the level an image is written at, as its resource says (zero: the base level) */
static struct air_level_place
image_level(const struct air_resource *r)
{
   if (r && r->level_from_buffer)
      return (struct air_level_place){ .buffer = r->level_buffer, .offset = r->level_offset,
                                       .bytes = r->level_bytes, .is_float = r->level_float,
                                       .bias = r->level_bias };
   return (struct air_level_place){ .constant = true, .value = r ? r->level : 0 };
}

static bool
image_at_level(struct air_ctx *c, struct air_resource *r, const struct air_level_place *lp,
               const char *what, unsigned *binding)
{
   struct air_level_place had = image_level(r);
   if (r->level_written && !level_place_equal(&had, lp)) {
      /* another level of the same image: a slot of its own for that level (air_image_alias),
       * found again for every write at it */
      unsigned of = r->alias ? r->alias_of : r->index;
      for (unsigned i = 0; i < c->info->num_resources; i++) {
         struct air_resource *a = &c->info->resources[i];
         struct air_level_place at = image_level(a);
         if (a->kind == AIR_RESOURCE_IMAGE && a->level_written &&
             (a->index == of || (a->alias && a->alias_of == of)) && level_place_equal(&at, lp)) {
            *binding = a->index;
            return true;
         }
      }
      r = air_image_alias(c, of);
      if (!r)
         return false;
   }
   *binding = r->index;
   r->level_written = true;
   r->level = lp->constant ? (unsigned)lp->value : 0;
   r->level_from_buffer = !lp->constant;
   r->level_buffer = lp->buffer;
   r->level_offset = lp->offset;
   r->level_bytes = lp->bytes;
   r->level_float = lp->is_float;
   r->level_bias = lp->constant ? 0 : lp->bias;
   return true;
}

/* the { value, residency } struct these return */
static void
set_tex_result(struct air_ctx *c, const struct air_inst *in, nir_def *value)
{
   const struct air_type *ret = in->base.type;
   if (ret->kind != AIR_TYPE_STRUCT) {
      air_set(c, &in->base, air_val_ssa(c, value));
      return;
    }
   struct air_val *agg = rzalloc(c->mem_ctx, struct air_val);
   agg->kind = AIR_VAL_AGG;
   agg->num_elems = ret->num_members;
   agg->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(ret->num_members, 1));
   agg->elems[0] = air_val_ssa(c, value);
   for (unsigned i = 1; i < ret->num_members; i++) {
      /* the sparse-residency byte: nothing in the corpus reads it */
      unsigned bits = air_nir_bit_size(c, ret->members[i]);
      agg->elems[i] = air_val_ssa(c, nir_imm_intN_t(&c->b, 0, bits));
   }
   air_set(c, &in->base, agg);
}


/* A query: a size, a level count or a sample count, of a texture or an image. */
static nir_def *
query_texture(struct air_ctx *c, const struct air_ptr *tex, const struct air_tex_name *t,
              nir_texop op, nir_def *lod, unsigned comps)
{
   if (tex->is_image) {
      nir_def *index = tex->index ? nir_iadd_imm(&c->b, tex->index, tex->binding)
                                  : nir_imm_int(&c->b, tex->binding);
      switch (op) {
      case nir_texop_txs: {
         /* an image size has as many components as the image has dimensions;
          * the caller passes 0 for "as many as there are"
          */
         unsigned n = comps ? comps :
            (t->dim == GLSL_SAMPLER_DIM_1D || t->dim == GLSL_SAMPLER_DIM_BUF ? 1 :
             t->dim == GLSL_SAMPLER_DIM_3D ? 3 : 2) + (t->is_array ? 1 : 0);
         return nir_image_size(&c->b, n, 32, index, lod ? lod : nir_imm_int(&c->b, 0),
                               .image_dim = t->dim, .image_array = t->is_array);
      }
      case nir_texop_query_levels:
         return nir_image_levels(&c->b, 32, index, .image_dim = t->dim,
                                 .image_array = t->is_array);
      default:
         return nir_image_samples(&c->b, 32, index, .image_dim = t->dim,
                                  .image_array = t->is_array);
      }
   }

   nir_tex_instr *q = nir_tex_instr_create(c->nir, (lod ? 1 : 0) + (tex->index ? 1 : 0));
   q->op = op;
   q->sampler_dim = t->dim;
   q->is_array = t->is_array;
   q->dest_type = nir_type_int32;
   q->texture_index = tex->binding;
   q->sampler_index = 0;
   unsigned qn = 0;
   if (lod)
      q->src[qn++] = (nir_tex_src){ .src_type = nir_tex_src_lod, .src = nir_src_for_ssa(lod) };
   if (tex->index)
      q->src[qn++] = (nir_tex_src){ .src_type = nir_tex_src_texture_offset,
                                    .src = nir_src_for_ssa(tex->index) };
   /* NIR fixes the size of a query's result itself -- txs is two components
    * for a 2D texture whatever the caller asks one of them for -- so it is
    * asked here and the component picked afterwards.
    */
   if (comps == 0)
      comps = nir_tex_instr_dest_size(q);
   nir_def_init(&q->instr, &q->def, comps, 32);
   nir_builder_instr_insert(&c->b, &q->instr);
   return &q->def;
}

/* is every component of this value a constant zero? */
static bool
all_zero(nir_def *def)
{
   if (!nir_src_is_const(nir_src_for_ssa(def)))
      return false;
   for (unsigned i = 0; i < def->num_components; i++)
      if (nir_src_comp_as_int(nir_src_for_ssa(def), i) != 0)
         return false;
   return true;
}

/* The coordinate, with the array index that follows it folded into it.
 *
 * NIR wants one coordinate with the layer as its last component; AIR passes
 * the two separately (see the signatures above). Which way the layer is
 * converted follows the coordinate's own type, which is also the only place
 * the answer can come from: a sample's coordinate is floating point and NIR's
 * array layer is then a float like the rest of it, while a read's is an
 * integer and the layer stays one.
 *
 * The float conversion is exact and not a rounding to be worried about. Metal
 * hands over an integer layer; u2f of an integer below 2^24 is exact in a
 * float, and 2^24 layers is 16 million slices of a texture array whose limit
 * on this hardware is 2 048. What a backend does with a *fractional* layer --
 * Vulkan says round to even, GLSL says nearest -- therefore never arises
 * here, because the value handed to it is always a whole number.
 *
 * An integer coordinate is widened to 32 bits first so that the layer, which
 * arrives as i32, does not have to be narrowed to meet it: air.read_texture_2d
 * has a 16-bit coordinate variant and the layer in those signatures is 32-bit
 * all the same.
 */
static nir_def *
tex_coord_and_layer(struct air_ctx *c, const struct air_inst *in,
                    const struct air_tex_name *t, const struct air_name *n,
                    unsigned *arg, unsigned ncoord)
{
   if (in->num_operands <= *arg) {
      air_error(c, "intrinsic: air.%s without a coordinate", n->part[0]);
      return NULL;
   }
   const struct air_value *cv = in->operands[*arg];
   nir_def *coord = air_get_ssa(c, cv);
   if (!coord)
      return NULL;
   (*arg)++;
   bool is_float = air_type_is_float(air_type_scalar(cv->type));
   if (!is_float)
      coord = nir_u2u32(&c->b, coord);

   /* A cube read or written by texel: Metal's texturecube<T>::read(uint2 coord, uint face, ...)
    * and write(value, uint2 coord, uint face, ...) name the face as an argument of its own,
    * after the coordinate, as an array index is (ARKit's blur_cubemap and
    * compute_cubemap_from_latlong, Darwin 27). NIR addresses a cube image as (x, y, face). A
    * cube array would take the face and then the array index, the layer being 6 * index + face;
    * nothing in the corpus does that, so it is refused rather than assembled untried. */
   if (t->dim == GLSL_SAMPLER_DIM_CUBE && !is_float && coord->num_components == 2) {
      if (t->is_array) {
         air_error(c, "intrinsic: air.%s, a cube array addressed by face (not yet)", n->part[0]);
         return NULL;
      }
      if (in->num_operands <= *arg) {
         air_error(c, "intrinsic: air.%s without the face its signature has", n->part[0]);
         return NULL;
      }
      nir_def *face = air_get_ssa(c, in->operands[(*arg)++]);
      if (!face)
         return NULL;
      if (face->num_components != 1) {
         air_error(c, "intrinsic: air.%s with a %u-component face", n->part[0],
                   face->num_components);
         return NULL;
      }
      coord = nir_vec3(&c->b, nir_channel(&c->b, coord, 0), nir_channel(&c->b, coord, 1),
                       nir_u2u32(&c->b, face));
   }

   if (t->is_array) {
      if (in->num_operands <= *arg) {
         air_error(c, "intrinsic: air.%s without the array index its signature has",
                   n->part[0]);
         return NULL;
      }
      nir_def *layer = air_get_ssa(c, in->operands[(*arg)++]);
      if (!layer)
         return NULL;
      if (layer->num_components != 1) {
         air_error(c, "intrinsic: air.%s with a %u-component array index",
                   n->part[0], layer->num_components);
         return NULL;
      }
      layer = is_float ? nir_u2fN(&c->b, layer, coord->bit_size)
                       : nir_u2u32(&c->b, layer);

      nir_def *comps[4];
      unsigned nc = MIN2(coord->num_components, 3);
      for (unsigned i = 0; i < nc; i++)
         comps[i] = nir_channel(&c->b, coord, i);
      comps[nc] = layer;
      coord = nir_vec(&c->b, comps, nc + 1);
   }

   if (coord->num_components != ncoord) {
      air_error(c, "intrinsic: air.%s with a %u-component coordinate, wanting %u",
                n->part[0], coord->num_components, ncoord);
      return NULL;
   }
   return coord;
}

/* -- an explicit imageblock in a kernel ------------------------------------
 *
 * Threadgroup memory laid out a pixel per thread of the group's x by y, each pixel
 * imageblock_pixel_bytes of the layout the shader declares, from c->imageblock_base (nir_air.h).
 * The group's width is the workgroup size the runtime compiles the kernel for, so it folds.
 */
static nir_def *
imageblock_pixel_offset(struct air_ctx *c, nir_def *x, nir_def *y)
{
   nir_def *w = nir_channel(&c->b, nir_load_workgroup_size(&c->b), 0);
   nir_def *pix = nir_iadd(&c->b, nir_imul(&c->b, y, w), x);
   return nir_iadd_imm(&c->b, nir_imul_imm(&c->b, pix, c->info->imageblock_pixel_bytes),
                       c->imageblock_base);
}

/* air.imageblock_data(<2 x i16> coord, i32, i16) -> ptr addrspace(4): the pixel's data, for
 * the shader to load and store through as it likes. The two numbers after the coordinate are 0
 * in every call seen (the sample and the layer, by their widths) and refused otherwise. */
static bool
imageblock_data(struct air_ctx *c, const struct air_inst *in)
{
   /* a block its function constant leaves out (CMImaging's texture-style filters gate theirs)
    * is not there: what reaches it is the null pointer, as for any parameter left out */
   if (!c->info->imageblock_pixel_bytes) {
      air_set(c, &in->base, air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_NONE }));
      return true;
   }
   nir_def *coord = in->num_operands > 1 ? air_get_ssa(c, in->operands[1]) : NULL;
   if (!coord || coord->num_components != 2)
      return coord ? air_error(c, "intrinsic: air.imageblock_data with a %u-component "
                                  "coordinate", coord->num_components) : false;
   for (unsigned i = 2; i < in->num_operands; i++) {
      uint64_t v;
      if (!air_const_uint(in->operands[i], &v) || v)
         return air_error(c, "intrinsic: air.imageblock_data with operand %u not zero", i);
   }
   coord = nir_u2u32(&c->b, coord);
   struct air_ptr p = { .class = AIR_PTR_SHARED,
                        .offset = imageblock_pixel_offset(c, nir_channel(&c->b, coord, 0),
                                                          nir_channel(&c->b, coord, 1)),
                        .pointee = in->base.type ? in->base.type->elem : NULL };
   air_set(c, &in->base, air_val_ptr(c, p));
   return true;
}

/* air.write_imageblock_slice_to_texture_2d.<coord>.<value>(tex, slice, i1, origin, size, dst,
 * lod, i1, i32): the size.x by size.y pixels of one member of the block (the slice: a pixel's
 * data pointer, imageblock_data at (0, 0) plus the member's offset) from origin on, written to
 * the texture at dst on. Metal runs it once for the group -- the shaders call it from one thread
 * after a barrier -- so here it is that thread's loop over the pixels. */
static bool
imageblock_slice_write(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   if (!c->info->imageblock_pixel_bytes)
      return true;   /* (a block left out: nothing to write, as imageblock_data says) */
   if (in->num_operands < 8)
      return air_error(c, "intrinsic: air.%s with %u operands", n->part[0], in->num_operands);
   /* the member's type, from the name's last part: [vN]{f,i}{16,32} */
   const char *vt = n->part[n->num - 1];
   unsigned count = 1, bits = 0;
   char kind = 0;
   if (sscanf(vt, "v%u%c%u", &count, &kind, &bits) != 3) {
      count = 1;
      if (sscanf(vt, "%c%u", &kind, &bits) != 2)
         kind = 0;
   }
   static const struct air_type half = { .kind = AIR_TYPE_HALF };
   static const struct air_type flt = { .kind = AIR_TYPE_FLOAT };
   static const struct air_type i16 = { .kind = AIR_TYPE_INT, .bits = 16 };
   static const struct air_type i32 = { .kind = AIR_TYPE_INT, .bits = 32 };
   const struct air_type *scalar = kind == 'f' ? (bits == 16 ? &half : bits == 32 ? &flt : NULL)
                                 : kind == 'i' ? (bits == 16 ? &i16 : bits == 32 ? &i32 : NULL)
                                               : NULL;
   if (!scalar || count < 1 || count > 4)
      return air_error(c, "intrinsic: air.%s of %s pixels", n->part[0], vt);
   struct air_type *vec = rzalloc(c->mem_ctx, struct air_type);
   *vec = (struct air_type){ .kind = AIR_TYPE_VECTOR, .count = count, .elem = scalar };
   const struct air_type *et = count == 1 ? scalar : vec;

   struct air_ptr tex;
   if (!tex_handle(c, in, 1, AIR_PTR_TEXTURE, &tex))
      return false;
   if (tex.binding == AIR_NULL_BINDING)
      return true;
   if (!tex.is_image)
      return air_error(c, "intrinsic: air.%s to a texture the interface says is not written",
                       n->part[0]);
   struct air_val *slice = air_get(c, in->operands[2]);
   if (!slice || slice->kind != AIR_VAL_PTR || slice->ptr.class != AIR_PTR_SHARED)
      return air_error(c, "intrinsic: air.%s of a slice that is not the imageblock's",
                       n->part[0]);
   nir_def *origin = air_get_ssa(c, in->operands[4]), *size = air_get_ssa(c, in->operands[5]);
   nir_def *dst = air_get_ssa(c, in->operands[6]);
   if (!origin || !size || !dst)
      return false;
   /* a size left undefined is the whole block: the group's x by y (CMImaging's write) */
   const struct air_constant *sk = air_value_as_constant(in->operands[5]);
   if (sk && (sk->ckind == AIR_CONST_UNDEF || sk->ckind == AIR_CONST_POISON))
      size = nir_trim_vector(&c->b, nir_load_workgroup_size(&c->b), 2);
   struct air_level_place lp;
   if (!level_place(c, in->operands[7], &lp))
      return air_error(c, "intrinsic: air.%s to a mip level the shader computes", n->part[0]);
   struct air_resource *r = image_resource(c, tex.binding);
   if (!r || !image_at_level(c, r, &lp, n->part[0], &tex.binding))
      return r ? false : air_error(c, "intrinsic: air.%s to image %u, which has no resource",
                                   n->part[0], tex.binding);
   origin = nir_u2u32(&c->b, origin);
   size = nir_u2u32(&c->b, size);
   dst = nir_u2u32(&c->b, dst);

   /* the member's own offset in a pixel: the slice's offset less pixel (0, 0)'s */
   nir_def *member = nir_isub(&c->b, slice->ptr.offset ? nir_u2u32(&c->b, slice->ptr.offset)
                                                       : nir_imm_int(&c->b, 0),
                              nir_imm_int(&c->b, (int)c->imageblock_base));
   nir_builder at = nir_builder_at(nir_before_impl(c->b.impl));
   nir_def *ry = nir_decl_reg(&at, 1, 32, 0), *rx = nir_decl_reg(&at, 1, 32, 0);
   nir_store_reg(&c->b, nir_imm_int(&c->b, 0), ry);
   struct air_loop ly, lx;
   air_loop_head(c, &ly);
   air_loop_test(c, &ly, nir_ult(&c->b, nir_load_reg(&c->b, ry), nir_channel(&c->b, size, 1)));
   nir_store_reg(&c->b, nir_imm_int(&c->b, 0), rx);
   air_loop_head(c, &lx);
   air_loop_test(c, &lx, nir_ult(&c->b, nir_load_reg(&c->b, rx), nir_channel(&c->b, size, 0)));
   nir_def *y = nir_load_reg(&c->b, ry), *x = nir_load_reg(&c->b, rx);
   nir_def *off = nir_iadd(&c->b, imageblock_pixel_offset(c,
                                     nir_iadd(&c->b, nir_channel(&c->b, origin, 0), x),
                                     nir_iadd(&c->b, nir_channel(&c->b, origin, 1), y)),
                           member);
   struct air_ptr src = { .class = AIR_PTR_SHARED, .offset = off, .pointee = et };
   nir_def *v = air_load_ptr(c, src, et, 8);
   if (!v)
      return false;
   nir_def *to = nir_vec2(&c->b, nir_iadd(&c->b, nir_channel(&c->b, dst, 0), x),
                          nir_iadd(&c->b, nir_channel(&c->b, dst, 1), y));
   nir_image_store(&c->b, tex.index ? nir_iadd_imm(&c->b, tex.index, tex.binding)
                                    : nir_imm_int(&c->b, tex.binding),
                   nir_pad_vector(&c->b, to, 4), nir_imm_int(&c->b, 0),
                   nir_pad_vector(&c->b, v, 4), nir_imm_int(&c->b, 0),
                   .image_dim = GLSL_SAMPLER_DIM_2D, .image_array = false,
                   .src_type = (kind == 'f' ? nir_type_float : nir_type_uint) | v->bit_size);
   nir_store_reg(&c->b, nir_iadd_imm(&c->b, x, 1), rx);
   air_loop_end(c, &lx);
   nir_store_reg(&c->b, nir_iadd_imm(&c->b, nir_load_reg(&c->b, ry), 1), ry);
   air_loop_end(c, &ly);
   return true;
}

static bool
texture_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   /* texture.fence(): what this thread wrote to the texture is visible to its own reads after
    * it -- an image memory barrier, with no meeting of threads (Metal's own words) */
   if (!strncmp(n->part[0], "fence_texture_", 14)) {
      nir_barrier(&c->b, .execution_scope = SCOPE_NONE, .memory_scope = SCOPE_INVOCATION,
                  .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_image);
      return true;
   }
   struct air_tex_name t;
   if (!parse_tex_name(n->part[0], &t))
      return air_error(c, "intrinsic: air.%s (a texture name this cannot take apart)",
                       n->part[0]);
   if (t.is_buffer)
      return air_error(c, "intrinsic: air.%s (texture buffers, not yet)", n->part[0]);
   /* A comparison belongs to a depth texture and to a sample: Apple's header declares
    * sample_compare and gather_compare on depth2d and depth2d_array and on nothing
    * else, so a name claiming either of the other two shapes is a misparse and not a
    * gap. Said by name rather than translated into a plausible instruction.
    */
   if (t.compare && !t.depth)
      return air_error(c, "intrinsic: air.%s (a comparison of a colour texture, which "
                          "Metal declares on depth textures only)", n->part[0]);
   if (t.compare && !strcmp(t.op, "gather"))
      return air_error(c, "intrinsic: air.%s (gather_compare: four comparisons in one "
                          "instruction, which nothing measured here attests -- no module "
                          "of the 456 and none of Resident Evil 4's 7 879 calls one -- so "
                          "there is nothing to check an answer against)", n->part[0]);
   /* Only a sample has a gradient form -- Apple's header has
    * __metal_sample_*_t_grad and no gather, read or write counterpart -- and a
    * name that claimed one would be read by the sample branch's rules below,
    * which is the kind of silent misparse this file refuses instead.
    */
   if (t.grad && strcmp(t.op, "sample"))
      return air_error(c, "intrinsic: air.%s (a gradient form of %s, which Metal has no "
                          "signature for)", n->part[0], t.op);

   struct air_ptr tex;
   if (!tex_handle(c, in, 1, AIR_PTR_TEXTURE, &tex))
      return false;
   if (tex.binding == AIR_NULL_BINDING) {
      /* Metal's null texture: every read and sample of it is zero, a query is zero, a
       * write goes nowhere */
      if (!strcmp(t.op, "write"))
         return true;
      nir_alu_type zt = tex_dest_type(c, in->base.type, n);
      unsigned zbits = nir_alu_type_get_type_size(zt);
      if (!strcmp(t.op, "get")) {
         air_set(c, &in->base, air_val_ssa(c, nir_imm_intN_t(&c->b, 0, zbits)));
         return true;
      }
      /* as many components as the result has: a depth texture's sample and compare are one
       * (RealityKit's sampleShadowMap of a null depth2d stored a vec4 where a float belongs) */
      const struct air_type *rt = in->base.type;
      const struct air_type *vt = rt->kind == AIR_TYPE_STRUCT && rt->num_members ? rt->members[0] : rt;
      set_tex_result(c, in, nir_imm_zero(&c->b, MAX2(air_nir_components(vt), 1), zbits));
      return true;
   }

   unsigned arg = 2;   /* operands[0] is the callee, [1] the texture */
   struct air_ptr samp = { 0 };
   bool have_sampler = !strcmp(t.op, "sample") || !strcmp(t.op, "gather") ||
                       !strcmp(t.op, "lod");
   if (have_sampler) {
      if (!tex_handle(c, in, arg, AIR_PTR_SAMPLER, &samp))
         return false;
      arg++;
   }
   /* Darwin 26's AIR passes a read a sampler as well, the one air.get_read_sampler()
    * returns, and an offset after the coordinate:
    *   { <4 x half>, i8 } air.read_texture_2d.v4f16(ptr as(1) tex, ptr as(2) sampler,
    *                                                 <2 x i32> coord, <2 x i32> offset,
    *                                                 i32 lod, i32)
    * (QuartzCore's fixed_frag_* of 23G90). A texel fetch has no use for a sampler -- no
    * filter, no addressing beyond the texture's own size -- so it is passed over, and the
    * offset is taken when it is zero, which is every call seen; another is refused.
    */
   bool read_with_sampler = false;
   if (!strcmp(t.op, "read") && in->num_operands > arg &&
       in->operands[arg]->type->kind == AIR_TYPE_POINTER &&
       in->operands[arg]->type->addrspace == 2) {
      arg++;
      read_with_sampler = true;
   }

   /* A depth texture takes a plane or aspect selector before the coordinate,
    * and every call in the corpus passes 1. What else it can be is not known
    * from the data, so anything else is refused rather than assumed.
    */
   /* (the queries of a depth texture take none: air.get_num_mip_levels_depth_2d(ptr as(1)) and
    * air.get_width_depth_2d(ptr as(1), i32) -- VFX's quad_display_depth2D, which read the selector
    * past the end of its one operand and crashed) */
   if (t.depth && strcmp(t.op, "get")) {
      uint64_t plane;
      if (!tex_flag(c, in, arg, &plane))
         return false;
      if (plane != 1)
         return air_error(c, "intrinsic: air.%s with aspect %u (only 1 is understood)",
                          n->part[0], (unsigned)plane);
      arg++;
   }

   unsigned ncoord = coord_components_of(&t);
   nir_alu_type dest_type = tex_dest_type(c, in->base.type, n);
   unsigned dest_bits = nir_alu_type_get_type_size(dest_type);

   if (!strcmp(t.op, "get")) {
      /* the queries: a size, a level count or a sample count */
      nir_def *res;
      if (!strcmp(t.query, "num_samples")) {
         res = query_texture(c, &tex, &t, nir_texop_texture_samples, NULL, 1);
      } else if (!strcmp(t.query, "num_mip_levels")) {
         res = query_texture(c, &tex, &t, nir_texop_query_levels, NULL, 1);
      } else {
         nir_def *lod = nir_imm_int(&c->b, 0);
         struct air_level_place lp;
         struct air_resource *ir = tex.is_image ? image_resource(c, tex.binding) : NULL;
         /* An image's size at a level other than the base is the size of a view made at that
          * level, which is then the level the image is bound at (image_at_level): its own
          * level 0. VariableBlurrer asks for the size of the level it writes, before it writes. */
         bool at_view = ir && in->num_operands > arg && level_place(c, in->operands[arg], &lp) &&
                        !(lp.constant && lp.value == 0);
         if (at_view && !image_at_level(c, ir, &lp, n->part[0], &tex.binding))
            return false;
         if (in->num_operands > arg && !at_view) {
            nir_def *given = air_get_ssa(c, in->operands[arg]);
            if (!given)
               return false;
            lod = nir_u2u32(&c->b, given);
         }
         nir_def *size = query_texture(c, &tex, &t, nir_texop_txs, lod, 0);
         unsigned comp = !strcmp(t.query, "width") ? 0 :
                         !strcmp(t.query, "height") ? 1 :
                         !strcmp(t.query, "depth") ? 2 : size->num_components - 1;
         if (!strcmp(t.query, "array_size") && !t.is_array)
            return air_error(c, "intrinsic: air.%s on a texture that is not an array",
                             n->part[0]);
         if (comp >= size->num_components)
            return air_error(c, "intrinsic: air.%s asks for component %u of %u",
                             n->part[0], comp, size->num_components);
         res = nir_channel(&c->b, size, comp);
      }
      air_set(c, &in->base, air_val_ssa(c, nir_u2uN(&c->b, res, dest_bits)));
      return true;
   }

   if (!strcmp(t.op, "atomic")) {
      /* air.atomic_fetch_<op>_explicit_texture_2d(tex, coord, <2 x i16> zero, <4 x i32> value,
       * order, scope) -> <4 x i32> with what the texel held in .x: Metal's texture atomics,
       * on the R32 integer formats it allows them on. The vector after the coordinate is zero
       * in every call seen and is refused otherwise; the order and scope are relaxed and the
       * texture's, which is what an image atomic is. */
      nir_def *coord = tex_coord_and_layer(c, in, &t, n, &arg, ncoord);
      if (!coord)
         return false;
      nir_def *extra = in->num_operands > arg + 1 ? air_get_ssa(c, in->operands[arg]) : NULL;
      if (extra && extra->num_components == coord->num_components &&
          in->operands[arg + 1]->type->kind == AIR_TYPE_VECTOR) {
         if (!nir_src_is_const(nir_src_for_ssa(extra)) ||
             nir_src_comp_as_uint(nir_src_for_ssa(extra), 0) ||
             nir_src_comp_as_uint(nir_src_for_ssa(extra), extra->num_components - 1))
            return air_error(c, "intrinsic: air.%s with a second coordinate that is not zero",
                             n->part[0]);
         arg++;
      }
      nir_def *value = air_get_ssa(c, in->operands[arg++]);
      if (!value)
         return false;
      if (!tex.is_image)
         return air_error(c, "intrinsic: air.%s on a texture the interface says is not "
                             "written", n->part[0]);
      bool sgn = n->num > 2 && !strcmp(n->part[2], "s");
      nir_atomic_op op = !strcmp(t.atomic, "add") || !strcmp(t.atomic, "sub") ? nir_atomic_op_iadd
                       : !strcmp(t.atomic, "and") ? nir_atomic_op_iand
                       : !strcmp(t.atomic, "or") ? nir_atomic_op_ior
                       : !strcmp(t.atomic, "xor") ? nir_atomic_op_ixor
                       : !strcmp(t.atomic, "min") ? (sgn ? nir_atomic_op_imin : nir_atomic_op_umin)
                       : !strcmp(t.atomic, "max") ? (sgn ? nir_atomic_op_imax : nir_atomic_op_umax)
                                                  : nir_atomic_op_xchg;
      nir_def *v = nir_u2u32(&c->b, nir_channel(&c->b, value, 0));
      if (!strcmp(t.atomic, "sub"))
         v = nir_ineg(&c->b, v);
      nir_def *was = nir_image_atomic(&c->b, 32,
                                      tex.index ? nir_iadd_imm(&c->b, tex.index, tex.binding)
                                                : nir_imm_int(&c->b, tex.binding),
                                      nir_pad_vector(&c->b, nir_u2u32(&c->b, coord), 4),
                                      nir_imm_int(&c->b, 0), v,
                                      .image_dim = t.dim, .image_array = t.is_array,
                                      .format = sgn ? PIPE_FORMAT_R32_SINT : PIPE_FORMAT_R32_UINT,
                                      .atomic_op = op);
      const struct air_type *rt = in->base.type;
      unsigned rc = rt && rt->kind == AIR_TYPE_VECTOR ? (unsigned)rt->count : 1;
      nir_def *r = rc > 1 ? nir_pad_vector_imm_int(&c->b, was, 0, rc) : was;
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }

   if (!strcmp(t.op, "write")) {
      nir_def *coord = tex_coord_and_layer(c, in, &t, n, &arg, ncoord);
      if (!coord)
         return false;
      nir_def *value = air_get_ssa(c, in->operands[arg++]);
      if (!value)
         return false;
      if (!tex.is_image)
         return air_error(c, "intrinsic: air.%s to a texture the interface says is not "
                             "written", n->part[0]);
      /* the level: the view's, which the resource records (level_place) */
      struct air_level_place lp;
      if (!level_place(c, in->operands[arg], &lp))
         return air_error(c, "intrinsic: air.%s to a mip level the shader computes, where "
                             "an image is bound at one level", n->part[0]);
      struct air_resource *r = image_resource(c, tex.binding);
      if (!r)
         return air_error(c, "intrinsic: air.%s to image %u, which has no resource",
                          n->part[0], tex.binding);
      unsigned at = tex.binding;
      if (!image_at_level(c, r, &lp, n->part[0], &at))
         return false;
      if (at != tex.binding && tex.index)
         return air_error(c, "intrinsic: air.%s at a second mip level of an image taken out "
                             "of an array", n->part[0]);
      tex.binding = at;
      nir_image_store(&c->b, tex.index ? nir_iadd_imm(&c->b, tex.index, tex.binding)
                                       : nir_imm_int(&c->b, tex.binding),
                      nir_pad_vector(&c->b, nir_u2u32(&c->b, coord), 4),
                      nir_imm_int(&c->b, 0), nir_pad_vector(&c->b, value, 4),
                      nir_imm_int(&c->b, 0),
                      .image_dim = t.dim, .image_array = t.is_array,
                      .src_type = dest_type);
      return true;
   }

   /* sample, gather, read: a texture instruction, or an image load */
   nir_def *coord = tex_coord_and_layer(c, in, &t, n, &arg, ncoord);
   if (!coord)
      return false;

   if (read_with_sampler && in->num_operands > arg &&
       in->operands[arg]->type->kind == AIR_TYPE_VECTOR) {
      const struct air_constant *off = air_value_as_constant(in->operands[arg]);
      bool zero = off && off->ckind == AIR_CONST_NULL;
      if (off && off->ckind == AIR_CONST_DATA) {
         zero = true;
         for (unsigned i = 0; i < off->num_elems; i++)
            zero &= off->data[i] == 0;
      }
      if (!zero)
         return air_error(c, "intrinsic: air.%s with a texel offset that is not zero",
                          n->part[0]);
      arg++;
   }

   if (!strcmp(t.op, "read") && tex.is_image) {
      uint64_t extra = 0;
      /* A read at a level other than the base, or at one the shader takes out of a buffer
       * (VFX's vfx_reprojection_map reads and writes its map at the level its uniforms say):
       * the image is bound at that level, as a write to it is (image_at_level), and read at
       * level 0 of that view -- a view has one level. */
      struct air_level_place lp;
      struct air_resource *lr = !t.is_ms && arg < in->num_operands ? image_resource(c, tex.binding)
                                                                    : NULL;
      if (lr && level_place(c, in->operands[arg], &lp) &&
          (!lp.constant || lp.value || lr->level_written)) {
         unsigned at = tex.binding;
         if (!image_at_level(c, lr, &lp, n->part[0], &at))
            return false;
         if (at != tex.binding && tex.index)
            return air_error(c, "intrinsic: air.%s at a second mip level of an image taken "
                                "out of an array", n->part[0]);
         tex.binding = at;
      } else if (!tex_flag(c, in, arg, &extra))
         return false;
      nir_def *res = nir_image_load(&c->b, 4, dest_bits,
                                    tex.index ? nir_iadd_imm(&c->b, tex.index, tex.binding)
                                              : nir_imm_int(&c->b, tex.binding),
                                    nir_pad_vector(&c->b, nir_u2u32(&c->b, coord), 4),
                                    nir_imm_int(&c->b, t.is_ms ? extra : 0),
                                    nir_imm_int(&c->b, t.is_ms ? 0 : extra),
                                    .image_dim = t.dim, .image_array = t.is_array,
                                    .dest_type = dest_type);
      set_tex_result(c, in, res);
      return true;
   }

   /* The value a depth texel is compared against, which Metal passes straight after
    * the coordinate AND AFTER THE ARRAY INDEX -- __metal_sample_compare_depth_2d_t
    * takes (tex, samp, coord, compare_value, ...) and the _2d_array form takes (tex,
    * samp, coord, array, compare_value, ...), so the layer comes first in exactly the
    * place tex_coord_and_layer has already consumed it. Read here and not in the
    * branches below because it is before the offset pair in the plain form and before
    * the gradients in the _grad one: one position, not two.
    *
    * At the AIR level there is also the aspect selector the depth branch above ate, as
    * there is in air.sample_depth_2d -- Apple's header has no such argument and the
    * declaration in texturedFragment2DDepth does:
    *   { float, i8 } air.sample_depth_2d.f32(ptr as(1), ptr as(2), i32, <2 x float>,
    *                                         i1, <2 x i32>, i1, float, float, i32)
    * which is where the `i32` between the sampler and the coordinate comes from, and
    * the only reason the compare form's shape could be read off the header at all.
    */
   nir_def *compare = NULL;
   if (t.compare) {
      if (in->num_operands <= arg)
         return air_error(c, "intrinsic: air.%s without the value its signature compares "
                             "against", n->part[0]);
      if (!(compare = air_get_ssa(c, in->operands[arg++])))
         return false;
      if (compare->num_components != 1)
         return air_error(c, "intrinsic: air.%s compares against %u components, and a "
                             "depth comparison takes one", n->part[0],
                          compare->num_components);
      /* The texel and the value compared with it go into one register tuple on the
       * Adreno -- ir3 puts the comparison straight after the coordinates in src0 --
       * so a 16-bit comparison value beside 32-bit coordinates is a half register in a
       * full tuple. Metal's own signature is `float`, so this never fires on anything
       * Apple's compiler emits; it is here so that if something does, it is widened
       * and not mixed.
       */
      if (compare->bit_size != coord->bit_size)
         compare = nir_f2fN(&c->b, compare, coord->bit_size);
   }

   /* Seven and not six: a compare sample in the gradient form carries a coordinate, a
    * comparison value, an offset, two derivatives and a minimum level at once, which is
    * exactly six, and the next source anyone adds should not be the one that overflows
    * this silently.
    */
   nir_tex_src srcs[9];
   unsigned nsrc = 0;
   srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_coord,
                                 .src = nir_src_for_ssa(coord) };
   if (compare)
      srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_comparator,
                                    .src = nir_src_for_ssa(compare) };

   nir_texop op;
   if (!strcmp(t.op, "sample")) {
      /* The offset pair, for the dimensionalities that have one. A cube does
       * not, and reading one there consumed the level flag and the level and
       * left the minimum level to be read as the flags word -- which is the
       * refusal 621 of Resident Evil 4's modules used to earn, "argument 5 is
       * not a constant", from a float that indeed is not an integer constant.
       *
       * In the gradient form this pair comes AFTER the gradients, so the
       * reading is a small state machine over the signature rather than one
       * fixed order: see the table at the head of this section.
       */
      uint64_t has_offset = 0, is_level = 0;
      nir_def *offset = NULL, *ddx = NULL, *ddy = NULL, *min_lod = NULL, *level = NULL;

      if (t.grad) {
         ddx = air_get_ssa(c, in->operands[arg++]);
         ddy = air_get_ssa(c, in->operands[arg++]);
         min_lod = air_get_ssa(c, in->operands[arg++]);
         if (!ddx || !ddy || !min_lod)
            return false;
         if (!t.no_offset) {
            if (!tex_flag(c, in, arg, &has_offset))
               return false;
            arg++;
            if (!(offset = air_get_ssa(c, in->operands[arg++])))
               return false;
         }
      } else {
         if (!t.no_offset) {
            if (!tex_flag(c, in, arg, &has_offset))
               return false;
            arg++;
            if (!(offset = air_get_ssa(c, in->operands[arg++])))
               return false;
         }
         if (!tex_flag(c, in, arg, &is_level))
            return false;
         arg++;
         level = air_get_ssa(c, in->operands[arg++]);
         min_lod = air_get_ssa(c, in->operands[arg++]);
         if (!level || !min_lod)
            return false;
      }

      if (offset && has_offset && !all_zero(offset))
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_offset,
                                       .src = nir_src_for_ssa(nir_u2u32(&c->b, offset)) };

      if (t.grad) {
         /* An explicit pair of derivatives of the coordinate, which is what
          * Metal's sample(..., gradient2d(dPdx, dPdy)) is for: a shader that
          * knows better than the quad's own differences -- a ray-marched or
          * reprojected coordinate, or one sampled inside a branch the quad
          * does not take together. NIR has the operation (txd) and so does
          * the Adreno; ir3 emits it and softpipe's interpreter has TXD, so
          * unlike most of this family it can be both compiled and run here.
          *
          * The gradients carry the coordinate's components WITHOUT the array
          * layer, which is what NIR asks for too (nir_tex_instr_src_size:
          * coord_components minus is_array for ddx and ddy) and what AIR
          * passes -- <2 x float> for a 2d_array whose coordinate is also
          * <2 x float>. So nothing is added or dropped here, and that
          * agreement is the reason this needs no reshaping.
          */
         op = nir_texop_txd;
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_ddx,
                                       .src = nir_src_for_ssa(ddx) };
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_ddy,
                                       .src = nir_src_for_ssa(ddy) };
      } else {
         bool level_is_zero = nir_src_is_const(nir_src_for_ssa(level)) &&
                              nir_src_as_float(nir_src_for_ssa(level)) == 0.0;
         if (is_level) {
            op = nir_texop_txl;
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_lod,
                                          .src = nir_src_for_ssa(level) };
         } else if (!level_is_zero) {
            op = nir_texop_txb;
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_bias,
                                          .src = nir_src_for_ssa(level) };
         } else {
            op = nir_texop_tex;
         }
      }

      if (!(nir_src_is_const(nir_src_for_ssa(min_lod)) &&
            nir_src_as_float(nir_src_for_ssa(min_lod)) == 0.0))
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_min_lod,
                                       .src = nir_src_for_ssa(min_lod) };
   } else if (!strcmp(t.op, "gather")) {
      uint64_t has_offset = 0;
      nir_def *offset = NULL;
      if (!t.no_offset) {
         if (!tex_flag(c, in, arg, &has_offset))
            return false;
         arg++;
         if (!(offset = air_get_ssa(c, in->operands[arg++])))
            return false;
      }
      if (offset && has_offset && !all_zero(offset))
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_offset,
                                       .src = nir_src_for_ssa(nir_u2u32(&c->b, offset)) };
      op = nir_texop_tg4;
   } else {
      /* read: a fetch by integer coordinate, with a level or a sample. Neither
       * has to be a constant -- read_texture_2d_ms is called with a computed
       * sample index throughout AppleMetalGLRenderer.
       */
      nir_def *which = air_get_ssa(c, in->operands[arg++]);
      if (!which)
         return false;
      which = nir_u2u32(&c->b, which);
      op = t.is_ms ? nir_texop_txf_ms : nir_texop_txf;
      srcs[nsrc++] = (nir_tex_src){
         .src_type = t.is_ms ? nir_tex_src_ms_index : nir_tex_src_lod,
         .src = nir_src_for_ssa(which),
      };
   }

   /* a texture (and a sampler) out of an array at an index the shader computes (air_ptr.index) */
   if (tex.index)
      srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_texture_offset,
                                    .src = nir_src_for_ssa(tex.index) };
   if (have_sampler && samp.index)
      srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_sampler_offset,
                                    .src = nir_src_for_ssa(samp.index) };
   nir_tex_instr *tex_instr = nir_tex_instr_create(c->nir, nsrc);
   tex_instr->op = op;
   tex_instr->sampler_dim = t.dim;
   tex_instr->is_array = t.is_array;
   /* is_new_style_shadow because Metal's sample_compare returns ONE value, which the
    * declaration says too -- air.sample_compare_depth_2d.f32 returns { float, i8 }, not
    * { <4 x float>, i8 } the way a colour sample does. NIR's old-style shadow
    * replicates the comparison into four components and would leave the shader reading
    * component 0 of four where the signature has one, which is the same answer by
    * accident; this says what the operation is.
    */
   tex_instr->is_shadow = t.compare;
   tex_instr->is_new_style_shadow = t.compare;
   tex_instr->dest_type = dest_type;
   tex_instr->coord_components = ncoord;
   tex_instr->texture_index = tex.binding;
   tex_instr->sampler_index = have_sampler ? samp.binding : 0;
   if (op == nir_texop_tg4) {
      uint64_t component;
      if (!tex_flag(c, in, arg, &component))
         return false;
      tex_instr->component = component;
   }
   for (unsigned i = 0; i < nsrc; i++)
      tex_instr->src[i] = srcs[i];

   const struct air_type *value_type = in->base.type;
   if (value_type->kind == AIR_TYPE_STRUCT && value_type->num_members)
      value_type = value_type->members[0];

   /* The width of the result is NIR's to decide: a sample is four components
    * whatever the shader does with them. Metal's depth2d.sample() returns one
    * float, which is channel 0 of those four -- the same thing GLSL gets from
    * a depth texture read without a comparison.
    */
   unsigned nir_comps = nir_tex_instr_dest_size(tex_instr);
   nir_def_init(&tex_instr->instr, &tex_instr->def, nir_comps, dest_bits);
   nir_builder_instr_insert(&c->b, &tex_instr->instr);

   nir_def *res = &tex_instr->def;
   unsigned want = air_nir_components(value_type);
   if (want < nir_comps)
      res = nir_trim_vector(&c->b, res, want);
   else if (want > nir_comps)
      return air_error(c, "intrinsic: air.%s gives %u components, the shader wants %u",
                       n->part[0], nir_comps, want);
   set_tex_result(c, in, res);
   return true;
}

/* -- the dispatcher ----------------------------------------------------- */

static bool
texture_op_chosen(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   for (unsigned k = 1; k < MIN2(in->num_operands, 3u); k++) {
      struct air_val *v = air_get(c, in->operands[k]);
      if (!v || v->kind != AIR_VAL_PTR_SELECT)
         continue;
      struct air_val *regs = NULL;
      struct air_split sp;
      air_split_begin(c, v->def, &sp);
      for (unsigned side = 0; side < 2; side++) {
         if (side == 1)
            air_split_else(c, &sp);
         air_set(c, in->operands[k], v->elems[side]);
         if (!texture_op_chosen(c, in, n)) {
            air_set(c, in->operands[k], v);
            return false;
         }
         if (in->base.type->kind != AIR_TYPE_VOID)
            regs = air_split_keep(c, air_get(c, &in->base), regs);
      }
      air_split_end(c, &sp);
      air_set(c, in->operands[k], v);
      if (in->base.type->kind != AIR_TYPE_VOID)
         air_set(c, &in->base, air_split_load(c, regs));
      return true;
   }
   return texture_op(c, in, n);
}

bool
air_translate_call(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   if (!strncmp(name, "llvm.", 5))
      return llvm_op(c, in, name);

   /* the function a [[stage_in]] patch's control points are read with, which Metal makes */
   if (c->cp_fn && in->operands[0] == c->cp_fn)
      return air_control_point_call(c, in);
   if (strncmp(name, "air.", 4))
      return air_call_in_module(c, in, name);

   struct air_name n;
   if (!split_name(name, &n))
      return air_error(c, "intrinsic: %s (a name this cannot take apart)", name);

   if (!strcmp(n.part[0], "is_function_constant_defined"))
      return fc_defined_op(c, in);
   if (n.num >= 2 && !strcmp(n.part[0], "wg") && !strcmp(n.part[1], "barrier"))
      return barrier_op(c, in, SCOPE_WORKGROUP);
   /* air.simdgroup.barrier: the same flags, over the SIMD group (MPS's image kernels, 105 of
    * MPSImage's 1 837) */
   if (n.num >= 2 && !strcmp(n.part[0], "simdgroup") && !strcmp(n.part[1], "barrier"))
      return barrier_op(c, in, SCOPE_SUBGROUP);
   /* air.is_uniform.<type>(x): whether every active lane holds the same x */
   if (!strcmp(n.part[0], "is_uniform") && in->num_operands > 1) {
      nir_def *x = air_get_ssa(c, in->operands[1]);
      if (!x)
         return false;
      if (x->bit_size == 1)
         x = nir_b2i32(&c->b, x);
      air_set(c, &in->base, air_val_ssa(c, nir_vote_ieq(&c->b, 1, x)));
      return true;
   }
   if (!strcmp(n.part[0], "atomic"))
      return atomic_op(c, in, &n);
   /* air.get_num_samples(): the render target's sample count, which the pipeline fixes and hands the
    * translation (air_to_nir_options.sample_count) -- RealityKit's occlusion and overdraw shaders */
   if (!strcmp(n.part[0], "get_num_samples")) {
      unsigned samples = c->options && c->options->sample_count ? c->options->sample_count : 1;
      air_set(c, &in->base, air_val_ssa(c, nir_imm_intN_t(&c->b, samples,
                                                          air_nir_bit_size(c, in->base.type))));
      return true;
   }
   /* air.load.device_coherent / air.store.device_coherent: a load or a store another threadgroup
    * may be reading or writing at the same time (MPSCore's single_pass_scan publishes partial
    * sums so). A plain load or store of the place, made visible by the barriers and the atomics
    * around it, which is how the shader orders them. */
   if (n.num >= 2 && !strcmp(n.part[1], "device_coherent") &&
       (!strcmp(n.part[0], "load") || !strcmp(n.part[0], "store"))) {
      bool is_load = !strcmp(n.part[0], "load");
      unsigned at = is_load ? 1 : 2;
      struct air_val *pv = in->num_operands > at ? air_get(c, in->operands[at]) : NULL;
      if (!pv)
         return in->num_operands > at ? false
                                      : air_error(c, "intrinsic: %s with no pointer", name);
      if (pv->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: %s through something that is not a pointer", name);
      if (is_load) {
         nir_def *r = air_load_ptr(c, pv->ptr, in->base.type, 4);
         if (!r)
            return false;
         air_set(c, &in->base, air_val_ssa(c, r));
         return true;
      }
      nir_def *v = air_get_ssa(c, in->operands[1]);
      return v && air_store_ptr(c, pv->ptr, v, 4);
   }
   if (!strcmp(n.part[0], "discard_fragment"))
      return discard_op(c, in);
   /* Darwin 26: the sampler handed to every texel read (texture_op passes it over). A
    * sampler class with no binding: anything else that tried to sample through it would
    * find no sampler behind it, and nothing seen does.
    */
   /* Darwin 26: the "null texture" a shader puts where a texture its function constants leave
    * out would be (QuartzCore's fixed_frag_*: phi [null, the parameter] on fc_has_attachment_N).
    * A texture with no binding (AIR_NULL_BINDING): the phi takes the other side
    * (resolve_pointer_phi), and anything that does read it reads zero (texture_op).
    */
   /* Darwin 26: a function out of a visible function table (RenderBox's uber_fragment,
    * behind its has_function_table constant). A table the pipeline's constants leave out
    * is the null pointer (translate_input), and what comes out of it is a null function,
    * which a call through then answers with undef (air_to_nir.c, AIR_OP_CALL): that code
    * is dead under the same constant and is translated all the same. A table that IS
    * there needs the pipeline's linked functions, which this translator does not take.
    */
   if (!strcmp(n.part[0], "get_function_pointer_visible_function_table")) {
      struct air_val *table = in->num_operands > 1 ? air_get(c, in->operands[1]) : NULL;
      if (!table)
         return false;
      if (table->kind == AIR_VAL_PTR && table->ptr.class == AIR_PTR_NONE) {
         air_set(c, &in->base, air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_NONE }));
         return true;
      }
      /* A table the pipeline binds (CoreImage's ubershader): the handle's low word at
       * 8 * index, and the function the choice among the pipeline's linked functions by it
       * (nir_air.h, function_tables) -- each a placeholder that becomes the call of that
       * linked function where it is called (air_call_table_function). */
      if (table->kind != AIR_VAL_PTR || table->ptr.class != AIR_PTR_SSBO)
         return air_error(c, "intrinsic: air.get_function_pointer_visible_function_table of "
                             "something that is not a table");
      const struct air_to_nir_options *o = c->options;
      if (!o || !o->num_linked)
         return air_error(c, "intrinsic: air.get_function_pointer_visible_function_table, and "
                             "the pipeline links no functions");
      nir_def *index = in->num_operands > 2 ? air_get_ssa(c, in->operands[2]) : NULL;
      if (!index)
         return false;
      static const struct air_type u32 = { .kind = AIR_TYPE_INT, .bits = 32 };
      struct air_ptr at = air_ptr_add(c, table->ptr, nir_imul_imm(&c->b, nir_u2u32(&c->b, index), 8));
      nir_def *handle = air_load_ptr(c, at, &u32, 4);
      if (!handle)
         return false;
      struct air_val *chosen = NULL;
      for (int k = (int)o->num_linked - 1; k >= 0; k--) {
         struct air_function *ph = rzalloc(c->mem_ctx, struct air_function);
         ph->is_declaration = true;
         ph->base.kind = AIR_VALUE_FUNCTION;
         ph->base.name = ralloc_asprintf(c->mem_ctx, "%s.MTL_VISIBLE_FN_REF", o->linked[k].name);
         struct air_val *v = air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_NONE });
         v->fn = ph;
         chosen = chosen ? air_val_ptr_select(c, nir_ieq_imm(&c->b, handle, k + 1), v, chosen) : v;
      }
      air_set(c, &in->base, chosen);
      return true;
   }
   if (!strncmp(n.part[0], "get_null_texture", 16)) {
      air_set(c, &in->base, air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_TEXTURE,
                                                             .binding = AIR_NULL_BINDING }));
      return true;
   }
   /* Darwin 26: whether a texture parameter is the null one. Every texture here is
    * resolved while translating, so the answer is a constant: true of the null
    * texture (get_null_texture), false of anything bound.
    */
   if (!strncmp(n.part[0], "is_null_texture", 15)) {
      struct air_val *t = in->num_operands > 1 ? air_get(c, in->operands[1]) : NULL;
      if (!t)
         return t ? false : air_error(c, "intrinsic: air.is_null_texture of nothing");
      if (t->kind != AIR_VAL_PTR || t->ptr.class != AIR_PTR_TEXTURE)
         return air_error(c, "intrinsic: air.is_null_texture of something that is not "
                             "a resolved texture");
      air_set(c, &in->base, air_val_ssa(c, nir_imm_bool(&c->b,
                                                        t->ptr.binding == AIR_NULL_BINDING)));
      return true;
   }
   /* air.map_screen_to_physical_coordinates / map_physical_to_screen_coordinates(coord, map, layer):
    * through a rasterization rate map (RealityKit's foveated passes). The device has none to give
    * (-supportsRasterizationRateMapWithLayerCount: answers no), so every pass is rasterised at one
    * rate and screen and physical coordinates are the same: the coordinate as it came. */
   if (!strcmp(n.part[0], "map_screen_to_physical_coordinates") ||
       !strcmp(n.part[0], "map_physical_to_screen_coordinates")) {
      nir_def *coord = in->num_operands > 1 ? air_get_ssa(c, in->operands[1]) : NULL;
      if (!coord)
         return false;
      air_set(c, &in->base, air_val_ssa(c, coord));
      return true;
   }
   if (!strcmp(n.part[0], "get_read_sampler")) {
      air_set(c, &in->base, air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_SAMPLER,
                                                             .binding = AIR_NULL_BINDING }));
      return true;
   }

   if (!strcmp(n.part[0], "imageblock_data"))
      return imageblock_data(c, in);
   /* air.get_imageblock_width / _height: the block is the threadgroup's, a pixel a thread
    * (imageblock_pixel_offset lays it out so), so its size is the group's x and y (Portrait's
    * backgroundReplacement writes the block out by them) */
   if (!strcmp(n.part[0], "get_imageblock_width") || !strcmp(n.part[0], "get_imageblock_height")) {
      if (c->nir->info.stage != MESA_SHADER_COMPUTE)
         return air_error(c, "intrinsic: air.%s outside a kernel", n.part[0]);
      nir_def *size = nir_channel(&c->b, nir_load_workgroup_size(&c->b),
                                  !strcmp(n.part[0], "get_imageblock_height"));
      air_set(c, &in->base, air_val_ssa(c, nir_u2uN(&c->b, size, air_nir_bit_size(c, in->base.type))));
      return true;
   }
   if (!strncmp(n.part[0], "write_imageblock_slice_to_texture_", 34))
      return imageblock_slice_write(c, in, &n);

   bool handled = false;
   /* before math_op, which resolves every argument as a number: these take a
    * lane number or nothing at all, and air.simd_sum falls through to it
    */
   if (!simd_op(c, in, &n, &handled))
      return false;
   if (handled)
      return true;

   if (!math_op(c, in, &n, &handled))
      return false;
   if (handled)
      return true;

   const char *op = n.part[0];
   if (strstr(op, "texture") || strstr(op, "_depth_") || !strncmp(op, "sample", 6) ||
       !strncmp(op, "gather", 6) || !strncmp(op, "read_", 5) || !strncmp(op, "write_", 6) ||
       strstr(op, "_lod_"))
      return texture_op_chosen(c, in, &n);

   return air_error(c, "intrinsic: %s", name);
}
