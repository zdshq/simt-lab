#!/usr/bin/env python3
# Generate exact rational reference vectors independently of RTL alignment and host FMA.
# 独立于 RTL 对齐算法和宿主 FMA，生成精确有理数参考向量。
import argparse
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from numeric_reference import binary_value, rounded


def finite_value(bits):
    return binary_value(bits, 23, 8, 127)


def expected(a, b, c):
    value = finite_value(a)*finite_value(b)+finite_value(c)
    if value:
        return rounded(value)
    product_zero = not (a & 0x7fffffff) or not (b & 0x7fffffff)
    negative_zero = product_zero and (a^b) >> 31 and c == 0x80000000
    return 0x80000000 if negative_zero else 0


def operand(rng):
    exponents = (0, 1, 2, 16, 64, 100, 125, 126, 127, 128, 150, 200, 253, 254)
    fraction = rng.choice((0, 1, 2, 3, 0x3fffff, 0x400000, 0x7ffffe, 0x7fffff, rng.getrandbits(23)))
    return (rng.randrange(2) << 31) | (rng.choice(exponents) << 23) | fraction


# Cancellation inputs use an independently rounded exact product, including adjacent representable values.
# 抵消输入使用独立舍入的精确乘积，并包含相邻可表示数值。
def vectors():
    rng = random.Random(73019)
    for index in range(12000):
        a, b, c = (operand(rng) for _ in range(3))
        if index % 3 == 0:
            product = rounded(finite_value(a)*finite_value(b))
            if product & 0x7f800000 != 0x7f800000:
                c = (product ^ 0x80000000) + rng.choice((-1, 0, 1))
                c &= 0xffffffff
                if c & 0x7f800000 == 0x7f800000: c = 0
        yield a, b, c, expected(a, b, c)


def run_application():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    arguments = parser.parse_args()
    arguments.out.write_text(''.join(' '.join(f'{value:08x}' for value in item)+'\n' for item in vectors()))


if __name__ == '__main__':
    run_application()
