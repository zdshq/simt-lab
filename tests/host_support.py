# Host tests drive the persistent C++ process through the same public API as applications.
# 主机测试通过与应用相同的公开接口驱动持久 C++ 进程。
import os
from pathlib import Path
import unittest
from simt_lab import Program, Z
from simt_lab.host import Device


# Increment one uint32 per thread; arguments are pointer, element count, and increment.
# 每线程增加一个 uint32；参数为指针、元素数量和增量。
def increment_kernel(delay=0):
    p = Program().emit('S2R',rd=0,imm=1)
    for register in range(1,4): p.emit('LDC',rd=register,ra=Z,imm=(register-1)*4,mode=4)
    p.emit('S2R',rd=6,imm=3).emit('S2R',rd=7,imm=5)
    p.emit('IMAD',rd=0,ra=6,rb=7,rc=0).emit('ISETP',rd=0,ra=0,rb=2,mode=2)
    p.emit('MOVI',rd=4,imm=4).emit('IMAD',rd=4,ra=0,rb=4,rc=1)
    p.emit('LDG',rd=5,ra=4,mode=4,guard=0)
    for _ in range(delay): p.emit('NOP')
    p.emit('IADD',rd=5,ra=5,rb=3).emit('STG',ra=4,rb=5,mode=4,guard=0).emit('EXIT')
    return p


class HostCase(unittest.TestCase):
    # Cleanup aborts only the test-owned process, even when a deliberate job error remains.
    # 即使刻意制造了任务错误，清理也只中止测试持有的进程。
    def device(self, **options):
        runner = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        device = Device(runner,**options)
        self.addCleanup(lambda: device.close(wait=False))
        return device

    # Wait for actual engine overlap without assuming that a copy completes in one service tick.
    # 等待实际引擎重叠，不假定拷贝在一个服务 tick 内完成。
    def await_overlap(self, device):
        for _ in range(10000):
            stats = device.progress()
            if stats['compute'] and stats['copy']:
                return stats
        self.fail('compute and copy never overlapped')
