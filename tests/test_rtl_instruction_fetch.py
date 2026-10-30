# Real issue obtains machine words from the finite instruction caches, including Graph kernel changes.
# 实际发射从有限指令缓存取得机器字，包括 Graph 更换 kernel 的场景。
import unittest
from simt_lab import Program, Z
from support import SimulationCase
from host_support import HostCase


def check_fetch(test,hardware):
    dram=hardware['dram']
    test.assertEqual(dram['reads'],dram['data_reads']+dram['instruction_reads'])
    test.assertGreater(dram['instruction_reads'],0)
    decoders={(d['sm'],d['partition']):d for d in hardware['instruction_decoders']}
    for unit in hardware['instruction_fetch']:
        test.assertEqual((unit['backend'],unit['physical_partitions'],unit['bytes_per_partition'],unit['line_bytes']),('rtl',8,4096,64))
        test.assertEqual((unit['refill_ports'],unit['memory_backend'],unit['dram_backend']),(1,'shared_l2_dram','model'))
        test.assertEqual(unit['l2_backend'],hardware['l2']['backend'])
        test.assertGreaterEqual(unit['image_address'],1<<63)
        test.assertEqual(unit['clocks'],hardware['cycles']); test.assertFalse(unit['busy'])
        test.assertEqual(unit['memory_requests'],unit['memory_responses'])
        test.assertEqual(unit['memory_responses'],sum(p['fill_words'] for p in unit['partitions']))
        for part in unit['partitions']:
            test.assertEqual(part['address_errors'],0)
            test.assertEqual(part['queries'],part['delivered']+part['stalls'])
            test.assertEqual(part['reads'],part['hits']+part['misses'])
            test.assertEqual(part['fill_words'],4*part['misses'])
            decoder=decoders.get((unit['sm'],part['partition']))
            test.assertEqual(part['delivered'],decoder['inspected'] if decoder else 0)


class InstructionFetch(SimulationCase):
    execution_timeout=120

    def execute(self,program,*options,**kwargs):
        result=super().execute(program,'--timing','--register-rtl','--collector-rtl','--scoreboard-rtl','--hardware-trace',*options,**kwargs)
        if kwargs.get('error') is None: check_fetch(self,result['hardware'])
        return result

    def test_loop_reuses_cached_machine_words(self):
        p=Program().emit('MOVI',rd=0,imm=0).emit('MOVI',rd=1,imm=1).emit('MOVI',rd=2,imm=10)
        p.label('loop').emit('IADD',rd=0,ra=0,rb=1).emit('ISETP',rd=0,ra=0,rb=2,mode=2)
        p.emit('BRA',imm='loop',guard=0).emit('EXIT')
        result=self.execute(p,'--sms',1,'--partitions',1,'--threads',1)
        self.assertEqual(self.registers(result,0)[0],10)
        cache=result['hardware']['instruction_fetch'][0]['partitions'][0]
        self.assertEqual(cache['misses'],2); self.assertGreater(cache['hits'],10); self.assertGreater(cache['stalls'],0)

    def test_program_exceeds_cache_and_ends_in_partial_line(self):
        p=Program()
        for value in range(601): p.emit('MOVI',rd=0,imm=value)
        result=self.execute(p.emit('EXIT'),'--sms',1,'--partitions',1,'--threads',1)
        self.assertEqual(self.registers(result,0)[0],600)
        unit=result['hardware']['instruction_fetch'][0]
        self.assertEqual(unit['partitions'][0]['misses'],151)
        self.assertEqual(unit['memory_requests'],604)
        self.assertEqual(result['hardware']['dram']['instruction_reads'],302)
        self.assertEqual(result['hardware']['dram']['data_reads'],0)

    def test_conflicting_branch_targets_refill_tags(self):
        p=Program().emit('MOVI',rd=0,imm=0).emit('MOVI',rd=1,imm=1).emit('MOVI',rd=2,imm=4)
        p.label('head').emit('IADD',rd=0,ra=0,rb=1).emit('BRA',imm='far')
        for _ in range(251): p.emit('NOP')
        p.label('far').emit('ISETP',rd=0,ra=0,rb=2,mode=2).emit('BRA',imm='head',guard=0).emit('EXIT')
        result=self.execute(p,'--sms',1,'--partitions',1,'--threads',1)
        self.assertEqual(self.registers(result,0)[0],4)
        self.assertGreaterEqual(result['hardware']['instruction_fetch'][0]['partitions'][0]['misses'],8)

    def test_all_partitions_share_a_finite_refill_port(self):
        p=Program().emit('MOVI',rd=0,imm=17).emit('MOVI',rd=1,imm=25).emit('IADD',rd=2,ra=0,rb=1).emit('EXIT')
        for parts in (1,2,4,8):
            result=self.execute(p,'--sms',1,'--partitions',parts,'--threads',256)
            for warp in range(8): self.assertEqual(self.registers(result,2,warp=warp),[42]*32)
            unit=result['hardware']['instruction_fetch'][0]
            self.assertEqual(unit['memory_requests'],parts*4)
            self.assertEqual(result['hardware']['dram']['instruction_reads'],2)
            self.assertEqual([p['misses'] for p in unit['partitions']],[1]*parts+[0]*(8-parts))


class InstructionFetchHost(HostCase):
    def test_graph_changes_instruction_image_and_queries_are_readonly(self):
        device=self.device(timing=True,register_rtl=True,collector_rtl=True,scoreboard_rtl=True,hardware_trace=True)
        output=device.allocate(4)
        modules=[device.load(Program().emit('LDC',rd=1,ra=Z,mode=4).emit('MOVI',rd=0,imm=value)
                             .emit('STG',ra=1,rb=0,mode=4).emit('EXIT')) for value in (17,91)]
        graph=device.graph(); node=graph.add(modules[0].operation((output,),threads=1)); executable=graph.instantiate()
        for index in (0,1,0):
            executable.update(node,modules[index].operation((output,),threads=1))
            run=executable.launch(); run.wait()
            snapshot=run.node_event(node).query(); check_fetch(self,snapshot['hardware'])
            for _ in range(20): self.assertEqual(run.node_event(node).query(),snapshot)
            self.assertEqual(device.d2h(output,4).result(),(17 if index==0 else 91).to_bytes(4,'little'))
            self.assertEqual(device.stats()['hardware']['instruction_mappings'],0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
