# Share kernel construction, while keeping numeric expectations in a separate oracle.
# 复用 kernel 构造逻辑，数值期望保留在独立参考中。
from simt_lab import Program, Z
from simt_lab.tensor import store_words


# Initialize descriptors with one CTA leader, then synchronize all warps.
# 由一个 CTA leader 初始化描述符，再同步所有 warp。
def tensor_program(descriptor, columns=32, previous=None):
    p = Program().emit('S2R', rd=0, imm=1).emit('ISETP', rd=0, ra=0, rb=Z)
    p.emit('TMEM_ALLOC', rd=1, imm=columns, guard=0)
    store_words(p, 0, descriptor.words())
    if previous is not None:
        p.emit('MOVI', rd=2, imm=previous).emit('STT', ra=Z, rb=Z, rc=2, guard=0)
    return p.emit('BAR_SYNC', rd=0)


# Finish a single descriptor launch and make its TMEM results available to snapshots.
# 完成一次描述符执行，让快照能够检查 TMEM 结果。
def launch_matrix(descriptor, columns=32, previous=None):
    p = tensor_program(descriptor, columns, previous)
    return p.emit('MMA', ra=Z, guard=0).emit('BAR_SYNC', rd=0).emit('EXIT')
