# Reuse existing async contracts while requiring physical table traffic and hardware phase comparisons.
# 复用已有异步契约，同时要求物理事务表流量与硬件阶段比较。
import struct
import unittest
import test_timed_async
from support import SimulationCase, word
from host_support import HostCase
from simt_lab import Program, Z
from simt_lab.transport import DeviceError


class TransactionRtl:
    execution_timeout = 120

    def execute(self, program, *options, **kwargs):
        result = super().execute(program, '--register-rtl', '--collector-rtl', *options, **kwargs)
        if kwargs.get('error') is None:
            for sm in result['hardware']['async']:
                self.assertEqual(sm['transaction_backend'], 'rtl')
                table = sm['transaction_table']
                self.assertEqual((table['physical_slots'], table['record_bits'], table['occupancy']), (1024, 224, 0))
                self.assertEqual(table['commands'], table['responses'])
                self.assertEqual(table['phases'], sm['phase_completions'])
                self.assertEqual(table['errors'], 0)
                self.assertLessEqual(table['active_clocks'], table['clocks'])
            for warp in result['hardware']['warp_frontends']:
                self.assertEqual(warp['external_lanes'], 0)
                self.assertEqual(warp['async_blocked'], warp['async_woken'])
                self.assertGreaterEqual(warp['async_checked'], warp['async_blocked'])
        return result


class Contracts(TransactionRtl, test_timed_async.TimedAsyncTest):
    pass


class Pipeline(TransactionRtl, test_timed_async.DmaPipelineTest):
    pass


# Tail warps and multiple addresses share hardware lookup, while retired CTAs return all table records.
# 尾部 warp 与多个地址共享硬件查询，退休 CTA 归还全部表记录。
class TableCapacity(TransactionRtl, SimulationCase):
    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', *options, **kwargs)

    def test_all_records_and_cta_reuse(self):
        p = Program().emit('S2R', rd=0, imm=1).emit('ISETP', rd=0, ra=0, rb=Z)
        for address in range(0, 512, 32):
            p.emit('MOVI', rd=1, imm=address).emit('MB_INIT', ra=1, imm=1, guard=0)
            p.emit('MB_ARRIVE', rd=2, ra=1, guard=0).emit('BAR_SYNC', rd=0).emit('MB_WAIT', ra=1, rb=Z)
        p.emit('EXIT')
        result = self.execute(p, '--blocks', 7, '--threads', 37, '--max-ctas', 1, '--mbarrier-slots', 16)
        for cta in result['ctas']:
            for address in range(0, 512, 32): self.assertEqual(word(cta['shared'], address+4), 1)
        self.assertEqual(sum(sm['transaction_table']['publications'] for sm in result['hardware']['async']), 7*32)
        self.assertEqual(sum(sm['transaction_table']['commands'] for sm in result['hardware']['async']), 7*(32+32+1))

    def test_full_table_and_incomplete_retirement_errors(self):
        p = Program().emit('MB_INIT', ra=Z, imm=1).emit('MOVI', rd=1, imm=32).emit('MB_INIT', ra=1, imm=1).emit('EXIT')
        self.execute(p, '--threads', 1, '--mbarrier-slots', 1, error='mbarrier_table_full')
        p = Program().emit('MB_INIT', ra=Z, imm=1).emit('MB_EXPECT', ra=Z, imm=1).emit('EXIT')
        self.execute(p, '--threads', 1, error='incomplete_async_transaction')


class TransactionHost(HostCase):
    def test_graph_replays_after_failed_table_retirement(self):
        device = self.device(timing=True, register_rtl=True, collector_rtl=True, request_timeout=120)
        bad = Program().emit('MB_INIT', ra=Z, imm=1).emit('MB_EXPECT', ra=Z, imm=4).emit('EXIT')
        with self.assertRaisesRegex(DeviceError, 'incomplete_async_transaction'):
            device.load(bad).launch(threads=1).wait()
        buffer = device.allocate(8)
        p = Program().emit('LDC', rd=4, ra=Z, mode=4).emit('MOVI', rd=2, imm=128)
        p.emit('MB_INIT', ra=Z, imm=1).emit('MB_EXPECT', ra=Z, imm=4).emit('MB_ARRIVE', rd=3, ra=Z)
        p.emit('CP_ASYNC', ra=4, rb=2, rc=Z, imm=4).emit('MB_WAIT', ra=Z, rb=3)
        p.emit('LDS', rd=5, ra=2, mode=4).emit('STG', ra=4, rb=5, imm=4, mode=4).emit('EXIT')
        graph = device.graph(); upload = graph.add(device.op_h2d(buffer, struct.pack('<2I', 0xabcdef12, 0)))
        compute = graph.add(device.load(p).operation((buffer,), threads=1), (upload,))
        download = graph.add(device.op_d2h(buffer, 8), (compute,)); executable = graph.instantiate()
        for _ in range(2):
            run = executable.launch(stream=2)
            self.assertEqual(struct.unpack('<2I', run.node_result(download)), (0xabcdef12, 0xabcdef12))
            hardware = run.node_event(compute).query()['hardware']
            self.assertEqual(hardware['async'][0]['transaction_table']['occupancy'], 0)
            self.assertEqual(hardware['async'][0]['transaction_table']['phases'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
