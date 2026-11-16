#!/usr/bin/env python3
"""The lane arithmetic of air.simdgroup_matrix_8x8_multiply_accumulate (air_intrinsics.c), on 32
lanes simulated here, against an 8x8 matrix product done the plain way.

A thread of a simdgroup holds two elements of an 8x8 matrix: with l its lane, row
fm = 4 l4 + 2 l2 + l1 and columns fn, fn + 1 for fn = 4 l3 + 2 l0 (MLX's BaseMMAFrag::get_coord,
the layout Apple's thread_elements() has). The frontend reads row fm of A and column fn + j of B
from the lanes that hold them by shuffles; the lane numbers below are the frontend's, constant
for constant, so a slip in one of its masks or shifts is a wrong element here.

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
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
