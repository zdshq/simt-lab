# Use exact rationals rather than the simulator's floating-point operations.
# 使用精确有理数，不复用模拟器的浮点运算。
from fractions import Fraction


# Decode finite IEEE-like fields into an exact rational value.
# 将有限 IEEE 类字段解码为精确有理数。
def binary_value(bits, fraction_bits, exponent_bits, bias):
    fraction = bits & ((1 << fraction_bits) - 1)
    exponent = (bits >> fraction_bits) & ((1 << exponent_bits) - 1)
    if exponent == (1 << exponent_bits) - 1:
        raise ValueError('finite reference inputs required')
    significand = fraction + ((1 << fraction_bits) if exponent else 0)
    shift = (exponent or 1) - bias - fraction_bits
    magnitude = Fraction(significand << max(shift, 0), 1 << max(-shift, 0))
    return -magnitude if bits >> (fraction_bits + exponent_bits) else magnitude


# Divide with round-to-nearest, resolving exact ties toward an even result.
# 按最近值执行除法，精确中点舍入到偶数结果。
def nearest_even(numerator, denominator):
    quotient, remainder = divmod(numerator, denominator)
    return quotient + (2*remainder > denominator or (2*remainder == denominator and quotient % 2))


# Round an arbitrary rational directly to binary32 without a binary64 intermediate.
# 将任意有理数直接舍入到 binary32，不经过 binary64 中间值。
def rounded(value):
    if not value:
        return 0
    sign = 0x80000000 if value < 0 else 0
    numerator, denominator = abs(value).as_integer_ratio()
    exponent = numerator.bit_length() - denominator.bit_length()
    if Fraction(numerator, denominator) < Fraction(2) ** exponent:
        exponent -= 1
    shift = 149 if exponent < -126 else 23-exponent
    mantissa = nearest_even(numerator << max(shift, 0), denominator << max(-shift, 0))
    if exponent < -126:
        return sign | mantissa
    if mantissa == 1 << 24:
        mantissa >>= 1
        exponent += 1
    if exponent > 127:
        return sign | 0x7F800000
    return sign | ((exponent+127) << 23) | (mantissa - (1 << 23))


# Decode exactly what the device consumes, including packed low-precision inputs.
# 精确解码设备实际读取的内容，包括打包低精度输入。
def element(data, format, index):
    if format == 4:
        nibble = (data[index//2] >> (4*(index%2))) & 15
        magnitudes = (0, Fraction(1,2), 1, Fraction(3,2), 2, 3, 4, 6)
        return Fraction(-1 if nibble & 8 else 1) * magnitudes[nibble & 7]
    size = (4, 2, 2, 1)[format]
    bits = int.from_bytes(data[index*size:(index+1)*size], 'little')
    if format == 3:
        return bits if bits < 128 else bits - 256
    parameters = ((23, 8, 127), (10, 5, 15), (7, 8, 127))[format]
    return binary_value(bits, *parameters)


# Apply one exact fused product and one explicitly defined binary32 rounding.
# 计算一次精确融合乘加，并执行一次明确的 binary32 舍入。
def fused(a, b, accumulator):
    return rounded(a*b + binary_value(accumulator, 23, 8, 127))
