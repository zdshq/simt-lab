"""Bounded CUDA experiments and matching production-model probes.
有界的 CUDA 实验和对应的正式模型探针。
"""

STRIDES = (0, 1, 2, 4, 8, 16, 32, 33)
COALESCE = ((0, 0), (1, 0), (1, 1), (2, 0), (4, 0), (8, 0))


def chase(policy, kib, repeats, seed=123, carveout=0, iterations=8192):
    return ["chase", policy, kib * 1024, iterations, repeats, seed, carveout]


def cache_cases(full, repeats):
    sizes = (1, 4, 16, 24, 32, 48, 64, 80, 96, 112, 128, 160, 192, 256, 384, 512)
    seeds = (123, 20261030) if full else (123,)
    for seed in seeds:
        for kib in sizes if full else (4, 32, 64, 96, 128, 256, 512):
            yield chase("ca", kib, repeats, seed)
        if full:
            for carveout, points in ((64, (32, 48, 64, 80, 128)), (100, (16, 24, 32, 48, 64, 128))):
                for kib in points:
                    yield chase("ca", kib, repeats, seed, carveout)
        for kib in (256, 1024, 8192, 24576, 40960, 49152, 57344, 65536) if full else (4, 256, 1024):
            yield chase("cg", kib, repeats, seed)
        if full:
            for kib in (8192, 24576, 40960, 49152, 57344, 65536):
                yield [*chase("cg", kib, repeats, seed), 4096]


def native_cases(full, repeats):
    yield from cache_cases(full, repeats)
    for policy in ("ca", "cg"):
        for steps in (0, 4096, 16384):
            yield chase(policy, 4, repeats, iterations=steps)
    for stride in STRIDES:
        for steps in (0, 4096, 16384):
            yield ["shared", stride, steps, repeats]
    for stride, offset in COALESCE:
        yield ["coalesce", stride, offset, 64, 16, 128, repeats]


def model_cases():
    for stride, offset in COALESCE:
        yield ["coalesce", stride, offset]
    for stride in STRIDES:
        yield ["shared", stride, 4096]
    for policy in ("ca", "cg"):
        for kib in (4, 32, 64, 96, 128, 256, 384, 512):
            yield ["chase", policy, kib * 1024, 8192, 123]


def sanitizer_cases():
    yield chase("ca", 4, 1, iterations=0)
    yield chase("ca", 128, 1, iterations=513)
    yield chase("cg", 512, 1, iterations=513)
    yield [*chase("cg", 512, 1, iterations=513), 4096]
    for stride in (0, 32, 33):
        yield ["shared", stride, 513, 1]
    for stride, offset in ((0, 0), (1, 1), (8, 7)):
        yield ["coalesce", stride, offset, 3, 7, 513, 1]
