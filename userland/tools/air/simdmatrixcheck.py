#!/usr/bin/env python3
"""The lane arithmetic of air.simdgroup_matrix_8x8_multiply_accumulate (air_intrinsics.c), on 32
lanes simulated here, against an 8x8 matrix product done the plain way.

A thread of a simdgroup holds two elements of an 8x8 matrix: with l its lane, row
fm = 4 l4 + 2 l2 + l1 and columns fn, fn + 1 for fn = 4 l3 + 2 l0 (MLX's BaseMMAFrag::get_coord,
the layout Apple's thread_elements() has). The frontend reads row fm of A and column fn + j of B
from the lanes that hold them by shuffles; the lane numbers below are the frontend's, constant
for constant, so a slip in one of its masks or shifts is a wrong element here.

And the same through memory: simdgroup_matrix_8x8_load, init_diag and _store, each lane taking its
two elements at the frontend's addresses (element_at).

Softpipe has no shuffle (nir_to_tgsi has no subgroup operations), so this is the check of the
layout that can run without the phone.

    tools/air/simdmatrixcheck.py
"""
import random
import sys


def coord(l):
    qid = l // 4
    fm = (qid & 4) + ((l // 2) % 4)
    fn = (qid & 2) * 2 + (l % 2) * 2
    return fm, fn


def from_a(l, k):
    return (l & ~0x9) | ((k >> 2) << 3) | ((k >> 1) & 1)


def from_b(l, k):
    return (l & ~0x16) | ((k >> 2) << 4) | (((k >> 1) & 1) << 2) | ((k & 1) << 1)


def main():
    rnd = random.Random(7)
    bad = 0
    for trial in range(20):
        A = [[rnd.randint(-9, 9) for _ in range(8)] for _ in range(8)]
        B = [[rnd.randint(-9, 9) for _ in range(8)] for _ in range(8)]
        C = [[rnd.randint(-9, 9) for _ in range(8)] for _ in range(8)]
        want = [[C[i][j] + sum(A[i][k] * B[k][j] for k in range(8)) for j in range(8)]
                for i in range(8)]
        # each lane's two elements, for two simdgroups of a 64-lane wave
        for group in (0, 32):
            held = {}
            for l in range(32):
                fm, fn = coord(l)
                held[group + l] = ((A[fm][fn], A[fm][fn + 1]), (B[fm][fn], B[fm][fn + 1]),
                                   (C[fm][fn], C[fm][fn + 1]))
            seen = set()
            for l in range(32):
                lane = group + l
                fm, fn = coord(l)
                acc = list(held[lane][2])
                for k in range(8):
                    av = held[from_a(lane, k)][0][k & 1]
                    for j in range(2):
                        acc[j] += av * held[from_b(lane, k)][1][j]
                for j in range(2):
                    seen.add((fm, fn + j))
                    if acc[j] != want[fm][fn + j]:
                        bad += 1
                        if bad <= 5:
                            print("simdmatrixcheck: lane %d (%d, %d) is %d, want %d"
                                  % (lane, fm, fn + j, acc[j], want[fm][fn + j]))
            if len(seen) != 64:
                print("simdmatrixcheck: the lanes hold %d of the 64 elements" % len(seen))
                bad += 1
    print("simdmatrixcheck: an 8x8 product over 32 lanes by the frontend's shuffles: %s"
          % ("WRONG" if bad else "ok"))
    bad_mem = memory_round_trip(rnd)
    print("simdmatrixcheck: load, multiply and store through memory at an origin and a stride, "
          "plain and transposed: %s" % ("WRONG" if bad_mem else "ok"))
    return 1 if bad or bad_mem else 0


def element_at(origin, stride, r, c):
    """the element simdgroup_matrix_8x8_load and _store take for (r, c), as the frontend does"""
    return (origin[0] + c) * stride[0] + (origin[1] + r) * stride[1]


def memory_round_trip(rnd):
    """A and B loaded out of a 16-wide array at an origin (B transposed: its stride swapped), the
    product added to a matrix of init_diag(3), stored into another array, against numpy-free sums."""
    bad = 0
    L = 16
    src_a = [rnd.randint(-9, 9) for _ in range(L * L)]
    src_b = [rnd.randint(-9, 9) for _ in range(L * L)]
    oa, ob, od = (3, 2), (5, 1), (4, 6)
    sa, sb, sd = (1, L), (L, 1), (1, L)
    A = [[src_a[element_at(oa, sa, r, c)] for c in range(8)] for r in range(8)]
    B = [[src_b[element_at(ob, sb, r, c)] for c in range(8)] for r in range(8)]
    want = [[(3 if i == j else 0) + sum(A[i][k] * B[k][j] for k in range(8)) for j in range(8)]
            for i in range(8)]
    held = {}
    for l in range(32):
        fm, fn = coord(l)
        a = tuple(src_a[element_at(oa, sa, fm, fn + j)] for j in range(2))
        b = tuple(src_b[element_at(ob, sb, fm, fn + j)] for j in range(2))
        c = tuple(3 if fm == fn + j else 0 for j in range(2))
        held[l] = (a, b, c)
    out = [None] * (L * L)
    for l in range(32):
        fm, fn = coord(l)
        acc = list(held[l][2])
        for k in range(8):
            av = held[from_a(l, k)][0][k & 1]
            for j in range(2):
                acc[j] += av * held[from_b(l, k)][1][j]
        for j in range(2):
            out[element_at(od, sd, fm, fn + j)] = acc[j]
    for i in range(8):
        for j in range(8):
            got = out[element_at(od, sd, i, j)]
            if got != want[i][j]:
                bad += 1
                if bad <= 5:
                    print("simdmatrixcheck: stored (%d, %d) is %s, want %d" % (i, j, got, want[i][j]))
    return bad


if __name__ == "__main__":
    sys.exit(main())
