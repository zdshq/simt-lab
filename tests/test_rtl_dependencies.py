# Execute real kernels with the parallel query ports, including collectors and same-cycle writeback.
# 使用并行查询端口执行真实 kernel，包括收集器和同拍写回。
import unittest
from simt_lab import Program, Z
from simt_lab.transport import DeviceError
from support import SimulationCase
from host_support import HostCase


def check_queries(test, hardware):
    boards=hardware['scoreboards']
    for board in boards:
        test.assertEqual((board['backend'],board['issue_source_ports'],board['issue_predicate_ports']),('rtl',3,2))
        test.assertLessEqual(board['dependency_blocked'],board['dependency_queries'])
        for key in ('raw_conflicts','waw_conflicts','war_conflicts','predicate_conflicts'):
            test.assertLessEqual(board[key],board['dependency_blocked'])
    test.assertEqual(sum(b['dependency_queries'] for b in boards),sum(d['operands'] for d in hardware['instruction_decoders']))


class ParallelDependencies(SimulationCase):
    execution_timeout=120

    def execute(self, program, *options, **kwargs):
        result=super().execute(program,'--timing','--register-rtl','--collector-rtl','--scoreboard-rtl','--hardware-trace',*options,**kwargs)
        if kwargs.get('error') is None: check_queries(self,result['hardware'])
        return result

    def test_third_source_high_lane_and_slot_reuse(self):
        p=Program().emit('LDG',rd=62,ra=Z,mode=4).emit('IMAD',rd=2,ra=Z,rb=Z,rc=62).emit('EXIT')
        for parts in (1,2,4,8):
            result=self.execute(p,'--sms',1,'--partitions',parts,'--blocks',7,'--threads',37,'--max-ctas',1,
                                images={'global':(123).to_bytes(4,'little')})
            for cta in range(7):
                self.assertEqual(self.registers(result,2,block=cta,warp=0),[123]*32)
                self.assertEqual(self.registers(result,2,block=cta,warp=1)[:5],[123]*5)
            self.assertGreater(sum(b['raw_conflicts'] for b in result['hardware']['scoreboards']),0)

    def test_waw_and_uncollected_old_source(self):
        p=Program().emit('LDG',rd=0,ra=Z,mode=4).emit('MOVI',rd=0,imm=99).emit('EXIT')
        result=self.execute(p,'--threads',1)
        self.assertEqual(self.registers(result,0)[0],99)
        self.assertGreater(sum(b['waw_conflicts'] for b in result['hardware']['scoreboards']),0)
        p=Program().emit('MOVI',rd=0,imm=3).emit('MOVI',rd=4,imm=7).emit('MOVI',rd=8,imm=2)
        p.emit('IMAD',rd=16,ra=0,rb=4,rc=8).emit('MOVI',rd=8,imm=99).emit('EXIT')
        result=self.execute(p,'--threads',1)
        self.assertEqual((self.registers(result,16)[0],self.registers(result,8)[0]),(23,99))
        self.assertGreater(sum(b['war_conflicts'] for b in result['hardware']['scoreboards']),0)

    def test_predicate_destination_and_guard_order(self):
        p=Program().emit('ISETP',rd=0,ra=Z,rb=Z).emit('ISETP',rd=0,ra=Z,rb=Z,mode=1)
        p.emit('MOVI',rd=2,imm=44,guard=0).emit('MOVI',rd=2,imm=99,guard=8).emit('EXIT')
        result=self.execute(p,'--threads',32,'--int-latency',20)
        self.assertEqual(self.registers(result,2),[99]*32)
        self.assertGreater(sum(b['predicate_conflicts'] for b in result['hardware']['scoreboards']),0)
        self.assertGreater(sum(s['guard_stalls'] for s in result['hardware']['warp_schedulers']),0)

    def test_shuffle_cross_lane_dependencies_and_members(self):
        for guard in (7,0):
            p=Program().emit('S2R',rd=0,imm=0).emit('MOVI',rd=1,imm=16).emit('ISETP',rd=0,ra=0,rb=1,mode=2)
            p.emit('LDG',rd=2,ra=Z,mode=4,guard=8).emit('SHFL',rd=3,ra=2,rb=1,imm=0xffffffff,guard=guard).emit('EXIT')
            options={'images':{'global':(7).to_bytes(4,'little')}}
            if guard==0:
                self.execute(p,'--threads',32,error='membermask_not_subset',**options)
            else:
                result=self.execute(p,'--threads',32,**options)
                self.assertEqual(self.registers(result,3),[7]*32)
                self.assertGreater(sum(b['raw_conflicts'] for b in result['hardware']['scoreboards']),0)


class ParallelDependencyHost(HostCase):
    def test_query_observation_failure_and_graph_replay(self):
        device=self.device(timing=True,register_rtl=True,collector_rtl=True,scoreboard_rtl=True,hardware_trace=True)
        bad=device.load(Program().emit('LDG',rd=0,ra=Z,mode=4).emit('MOVI',rd=64,imm=1)).launch(threads=32,stream=1)
        with self.assertRaisesRegex(DeviceError,'register_out_of_range'): bad.wait()
        snapshot=bad.query()
        for _ in range(20): self.assertEqual(bad.query(),snapshot)
        module=device.load(Program().emit('MOVI',rd=62,imm=23).emit('IMAD',rd=0,ra=Z,rb=Z,rc=62).emit('EXIT'))
        graph=device.graph(); node=graph.add(module.operation(threads=37,blocks=5)); executable=graph.instantiate()
        for _ in range(2):
            run=executable.launch(stream=2); run.wait(); check_queries(self,run.node_event(node).query()['hardware'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
