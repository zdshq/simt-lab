# Reuse the functional contracts through the actual clocked DMA and Tensor paths.
# 通过真实逐拍 DMA 与 Tensor 通路复用功能契约。
import random
import unittest
import test_async as functional
import test_workloads as workloads
from support import SimulationCase, word
from simt_lab import Program, Z, NOT_PT


# Keep the record separate from every tested payload, including maximum-size commands.
# 记录与全部测试载荷隔离，包括最大尺寸的命令。
def copy_setup(size, source=64, destination=8192, arrive=True):
    p = Program().emit('S2R', rd=0, imm=1).emit('ISETP', rd=0, ra=0, rb=Z)
    p.emit('MOVI', rd=1, imm=1024).emit('MOVI', rd=2, imm=destination)
    p.emit('MOVI', rd=4, imm=source).emit('MB_INIT', ra=1, imm=1, guard=0)
    p.emit('MB_EXPECT', ra=1, imm=size, guard=0)
    if arrive:
        p.emit('MB_ARRIVE', rd=3, ra=1, guard=0)
    return p


def copy_command(p, size):
    return p.emit('CP_ASYNC', ra=4, rb=2, rc=1, imm=size, guard=0)


def finish_copy(p):
    return p.emit('MB_WAIT', ra=1, rb=3).emit('EXIT')


def event_cycles(report, action, pc=None):
    return [event['cycle'] for event in report['hardware']['trace']
            if event['action'] == action and (pc is None or event['pc'] == pc)]


def shared_bytes(report, address, size, block=0):
    memory = dict(report['ctas'][block]['shared'])
    return bytes(memory.get(address + offset, 0) for offset in range(size))


# Schedule a long conflicting store against record reservation in another warp.
# 让较长的冲突写入与另一 warp 的记录预约竞争。
def record_store_race(record_first):
    p = Program().emit('S2R', rd=0, imm=0).emit('ISETP', rd=0, ra=0, rb=Z)
    p.emit('S2R', rd=5, imm=2).emit('ISETP', rd=1, ra=5, rb=Z)
    p.emit('MOVI', rd=1, imm=8192).emit('MOVI', rd=6, imm=128)
    p.emit('IMAD', rd=2, ra=0, rb=6, rc=1).emit('BAR_SYNC', rd=0)
    p.emit('BRA', imm='writer', guard=1)
    # Keep the competing store and reservation in one instruction line.
    # 将竞争的写入与预约放在同一指令缓存行内。
    p.emit('MB_INIT', ra=1, imm=1, guard=0).emit('EXIT').label('writer')
    if record_first:
        for _ in range(4):
            p.emit('NOP')
    return p.emit('STS', ra=2, rb=Z, mode=4).emit('EXIT')


class TimedAsyncTest(functional.AsyncTest):
    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', '--hardware-trace', *options, **kwargs)


class TimedWorkloadTest(workloads.WorkloadTest):
    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', *options, **kwargs)


class DmaPipelineTest(SimulationCase):
    def execute(self, program, *options, **kwargs):
        return super().execute(program, '--timing', '--hardware-trace', *options, **kwargs)

    # Byte references include both guards and every payload byte, independent of simulator packing.
    # 字节参考包含两侧保护区和全部载荷，不依赖模拟器的打包实现。
    def check_bytes(self, size, source, destination, chunk, fragments):
        data = bytes((index*37 + 9) % 256 for index in range(source + size))
        p = copy_setup(size, source, destination)
        p.emit('MOVI', rd=5, imm=0xA5)
        p.emit('STS', ra=2, rb=5, imm=0xFFFFFFFF, mode=0, guard=0)
        p.emit('STS', ra=2, rb=5, imm=size, mode=0, guard=0)
        result = self.execute(finish_copy(copy_command(p, size)), '--async-chunk', chunk,
                              '--async-fragments', fragments, images={'global': data})
        self.assertEqual(shared_bytes(result, destination-1, size+2), b'\xa5'+data[source:]+b'\xa5')
        stats = result['hardware']['async'][0]
        self.assertEqual((stats['submitted'], stats['completed']), (1, 1))
        self.assertEqual((stats['read_bytes'], stats['write_bytes']), (size, size))
        self.assertGreater(result['hardware']['dram']['data_reads'], 0)

    def test_aligned_unaligned_tails_and_maximum_size(self):
        for size in (1, 3, 4, 31, 32, 127, 128, 129, 4096):
            for skew in (0, 1):
                with self.subTest(size=size, skew=skew):
                    self.check_bytes(size, 64+skew, 8192+skew, 128, 4)

    def test_random_fragment_boundaries(self):
        rng = random.Random(9017)
        for _ in range(20):
            self.check_bytes(rng.randint(1, 700), rng.randint(0, 79), rng.randint(8192, 8201),
                             rng.choice((1, 3, 4, 7, 16, 63, 128)), rng.choice((1, 2, 4, 8)))

    # Independent instructions overlap the copy, whereas consumers wait for published completion.
    # 独立指令与拷贝重叠，消费者则等待完成状态发布。
    def test_submission_retirement_and_consumer_order(self):
        p = copy_setup(256)
        copy_pc = len(p.instructions)
        copy_command(p, 256)
        alu_pc = len(p.instructions)
        p.emit('MOVI', rd=9, imm=123).emit('MB_WAIT', ra=1, rb=3)
        load_pc = len(p.instructions)
        p.emit('LDS', rd=8, ra=2, mode=4).emit('EXIT')
        result = self.execute(p, images={'global': bytes(range(256))*2})
        complete = event_cycles(result, 'copy_complete')[0]
        self.assertLess(event_cycles(result, 'retire', copy_pc)[0], complete)
        self.assertLess(event_cycles(result, 'retire', alu_pc)[0], complete)
        self.assertGreaterEqual(event_cycles(result, 'issue', load_pc)[0], complete)
        self.assertLess(max(event_cycles(result, 'copy_write_complete')), complete)
        self.assertEqual(self.registers(result, 8), [0x43424140]*32)

    def test_exit_waits_for_background_dma(self):
        p = copy_command(copy_setup(512), 512)
        exit_pc = len(p.instructions)
        p.emit('EXIT')
        result = self.execute(p)
        self.assertLess(event_cycles(result, 'retire', exit_pc)[0], event_cycles(result, 'copy_complete')[0])
        self.assertGreaterEqual(event_cycles(result, 'cta_retire')[0], event_cycles(result, 'copy_complete')[0])

    def test_full_queue_releases_completion_credits(self):
        p = copy_setup(1024)
        for index in range(4):
            p.emit('MOVI', rd=4, imm=64+index*256).emit('MOVI', rd=2, imm=8192+index*256)
            copy_command(p, 256)
        finish_copy(p)
        data = bytes(index % 251 for index in range(1088))
        reports = [self.execute(p, '--async-depth', depth, images={'global': data}) for depth in (1, 8)]
        for report in reports:
            self.assertEqual(shared_bytes(report, 8192, 1024), data[64:])
            self.assertEqual(report['hardware']['async'][0]['completed'], 4)
            self.assertEqual(word(report['ctas'][0]['shared'], 1028), 1)
        one, many = [report['hardware']['async'][0] for report in reports]
        self.assertEqual(one['peak_commands'], 1)
        self.assertGreater(one['command_stalls'], 0)
        self.assertGreater(many['peak_commands'], 1)

    def test_fragment_capacity_changes_throughput(self):
        p = finish_copy(copy_command(copy_setup(4096), 4096))
        reports = [self.execute(p, '--async-fragments', count) for count in (1, 8)]
        self.assertEqual([r['hardware']['async'][0]['fragments'] for r in reports], [32, 32])
        self.assertGreater(reports[0]['hardware']['cycles'], reports[1]['hardware']['cycles'])
        self.assertGreater(reports[0]['hardware']['async'][0]['fragment_stalls'], 0)

    # A completed payload cannot substitute for an outstanding arrival.
    # 已完成的载荷不能替代尚未到达的线程。
    def test_payload_completes_before_arrival(self):
        p = copy_command(copy_setup(4, arrive=False), 4)
        for _ in range(80):
            p.emit('IADD', rd=9, ra=9, rb=Z)
        p.emit('LDS', rd=7, ra=1, imm=4, mode=4)
        arrival_pc = len(p.instructions)
        p.emit('MB_ARRIVE', rd=3, ra=1, guard=0)
        result = self.execute(finish_copy(p))
        self.assertEqual(self.registers(result, 7), [0]*32)
        self.assertLess(event_cycles(result, 'copy_complete')[0], event_cycles(result, 'issue', arrival_pc)[0])
        self.assertEqual(word(result['ctas'][0]['shared'], 1028), 1)

    def test_two_arrivals_are_required(self):
        p = Program().emit('MB_INIT', ra=Z, imm=2).emit('MB_ARRIVE', rd=1, ra=Z)
        p.emit('LDS', rd=2, ra=Z, imm=4, mode=4).emit('MB_ARRIVE', rd=3, ra=Z)
        p.emit('MB_WAIT', ra=Z, rb=3).emit('EXIT')
        result = self.execute(p, '--threads', 1)
        self.assertEqual(self.registers(result, 2)[0], 0)
        self.assertEqual(word(result['ctas'][0]['shared'], 4), 1)

    def test_distinct_wait_addresses_share_the_control_engine(self):
        p = Program().emit('S2R', rd=0, imm=0).emit('ISETP', rd=0, ra=0, rb=Z)
        p.emit('MOVI', rd=1, imm=32)
        for address in (Z, 1):
            p.emit('MB_INIT', ra=address, imm=1, guard=0).emit('MB_ARRIVE', rd=3, ra=address, guard=0)
        p.emit('MOVI', rd=4, imm=1)
        p.emit('AND', rd=2, ra=0, rb=4).emit('IMUL', rd=2, ra=2, rb=1)
        p.emit('MB_WAIT', ra=2, rb=Z).emit('EXIT')
        reports = [self.execute(p, '--mbarrier-latency', latency) for latency in (1, 9)]
        for report in reports:
            self.assertEqual(report['hardware']['async'][0]['wait_queries'], 2)
        self.assertGreater(reports[1]['hardware']['cycles'], reports[0]['hardware']['cycles'])

    def test_empty_masks_do_not_allocate_or_access_memory(self):
        p = Program()
        for op in ('MB_INIT', 'MB_EXPECT', 'MB_ARRIVE', 'MB_WAIT', 'CP_ASYNC'):
            p.emit(op, ra=Z, guard=NOT_PT)
        result = self.execute(p.emit('EXIT'))
        self.assertEqual(result['hardware']['async'][0]['barrier_requests'], 0)
        self.assertEqual(result['hardware']['dram']['data_reads'], 0)

    def test_barrier_entries_are_released_on_cta_retirement(self):
        p = Program().emit('MB_INIT', ra=Z, imm=1).emit('MB_ARRIVE', rd=1, ra=Z).emit('EXIT')
        result = self.execute(p, '--threads', 1, '--blocks', 5, '--sms', 1,
                              '--max-ctas', 1, '--mbarrier-slots', 1)
        self.assertEqual(result['blocks_completed'], 5)

    def test_barrier_capacity_is_shared_across_resident_ctas(self):
        p = Program().emit('MB_INIT', ra=Z, imm=1)
        for _ in range(40):
            p.emit('IADD', rd=9, ra=9, rb=Z)
        p.emit('MB_ARRIVE', rd=1, ra=Z).emit('EXIT')
        self.execute(p, '--threads', 1, '--blocks', 2, '--sms', 1, '--shared-bytes', 256,
                     '--mbarrier-slots', 1, error='mbarrier_table_full')

    def test_new_record_cannot_overlap_pending_dma(self):
        p = copy_command(copy_setup(512), 512).emit('MB_INIT', ra=2, imm=1, guard=0)
        self.execute(p, error='mbarrier_overlaps_pending_copy')

    def test_record_reservation_protects_pending_and_future_stores(self):
        self.execute(record_store_race(False), '--threads', 64, '--partitions', 1, error='mbarrier_overlaps_pending_store')
        self.execute(record_store_race(True), '--threads', 64, '--mbarrier-latency', 64,
                     error='write_overlaps_mbarrier')

    def test_concurrent_ctas_keep_distinct_shared_payloads(self):
        p = copy_setup(256)
        p.emit('S2R', rd=5, imm=3).emit('MOVI', rd=6, imm=256).emit('IMAD', rd=4, ra=5, rb=6, rc=4)
        finish_copy(copy_command(p, 256))
        data = bytes((index//256*47 + index % 256) % 256 for index in range(1344))
        for policy in ('rr', 'min', 'max'):
            result = self.execute(p, '--sms', 1, '--blocks', 5, '--shared-bytes', 16384,
                                  '--async-depth', 2, '--warp-policy', policy, images={'global': data})
            self.assertEqual(result['hardware']['peak_ctas_per_sm'], 4)
            self.assertEqual(result['hardware']['async'][0]['completed'], 5)
            for block in range(5):
                self.assertEqual(shared_bytes(result, 8192, 256, block), data[64+block*256:320+block*256])

    def test_invalid_ranges_and_overlapping_records(self):
        for source, destination in ((0xFFFFFFFE, 8192), (64, 65534)):
            self.execute(copy_command(copy_setup(4, source, destination), 4), error='memory_out_of_bounds')
        p = Program().emit('MB_INIT', ra=Z, imm=1).emit('MOVI', rd=1, imm=16)
        self.execute(p.emit('MB_INIT', ra=1, imm=1), '--threads', 1, error='write_overlaps_mbarrier')

    def test_invalid_sizes_and_configuration(self):
        for size in (0, 4097):
            self.execute(copy_command(copy_setup(8192), size), error='invalid_async_copy_size')
        for count in (0, 1025):
            self.execute(Program().emit('MB_INIT', ra=Z, imm=count), '--threads', 1,
                         error='invalid_mbarrier_count')
        for option, value in (('--async-depth', 0), ('--async-fragments', 65), ('--async-chunk', 129),
                              ('--mbarrier-slots', 0), ('--mbarrier-latency', 0)):
            self.execute(Program().emit('EXIT'), option, value, error='invalid_async_pipeline_config')

    def test_expected_byte_and_arrival_overflow(self):
        p = copy_setup(0xFFFFFFFF).emit('MB_EXPECT', ra=1, imm=1, guard=0)
        self.execute(p, error='invalid_expected_bytes')
        p = copy_setup(4).emit('MB_ARRIVE', rd=3, ra=1, guard=0)
        self.execute(p, error='mbarrier_arrival_overflow')

    # Repeating a source after a phase wait uses cached bytes through the same hierarchy.
    # 阶段等待后重复同一来源，通过相同层次结构使用缓存字节。
    def test_repeated_source_uses_l1(self):
        p = copy_command(copy_setup(128), 128).emit('MB_WAIT', ra=1, rb=3)
        p.emit('MB_EXPECT', ra=1, imm=128, guard=0).emit('MB_ARRIVE', rd=3, ra=1, guard=0)
        p.emit('SHFL', rd=3, ra=3, rb=Z, imm=0xFFFFFFFF)
        result = self.execute(finish_copy(copy_command(p, 128)))
        self.assertEqual(result['hardware']['dram']['data_reads'], 4)
        self.assertGreaterEqual(result['hardware']['l1'][0]['hits'], 4)
        starts, ends = event_cycles(result, 'copy_submitted'), event_cycles(result, 'copy_complete')
        self.assertGreater(ends[0]-starts[0], ends[1]-starts[1])


if __name__ == '__main__':
    unittest.main(verbosity=2)
