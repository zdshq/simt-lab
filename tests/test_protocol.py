# Exercise the native process protocol without depending on the Python host wrappers.
# 不依赖 Python 主机封装，直接验证原生进程协议。
import json
import os
from pathlib import Path
import subprocess
import unittest


class ProtocolTest(unittest.TestCase):
    # Submit a complete conversation and require a clean process exit.
    # 提交完整会话，并要求进程正常退出。
    def conversation(self, requests, capacity=64):
        runner = Path(os.environ['SIMT_RUNNER']).with_name('simt-device')
        result = subprocess.run([str(runner),'2',str(capacity)],input='\n'.join([*requests,'QUIT'])+'\n',
                                text=True,capture_output=True,timeout=8)
        self.assertEqual(result.returncode,0,result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    # Publishing, fetching, and completing a packet are separately observable native states.
    # 发布、取走和完成包是可分别观察的原生状态。
    def test_native_queue_protocol(self):
        replies = self.conversation(['ALLOC 128','SUBMIT 0 0 H2D 65536 '+bytes([17]*128).hex(),
                                     'TICK 2','QUERY 1','BELL','TICK 1','QUERY 1','WAIT 1 10','STATS'])
        self.assertEqual(replies[2]['result']['read'],0)
        self.assertEqual(replies[3]['result']['state'],'queued')
        self.assertEqual(replies[5]['result']['read'],1)
        self.assertEqual(replies[6]['result']['state'],'running')
        self.assertEqual(replies[7]['result']['state'],'complete')
        self.assertEqual(replies[8]['result']['completed'],1)

    # Parse errors and allocation errors cannot partially consume a queue slot.
    # 解析错误和分配错误不能部分消耗队列位置。
    def test_native_recovery_after_invalid_requests(self):
        replies = self.conversation(['ALLOC 4','SUBMIT 0 0 H2D 65536 00 extra',
                                     'SUBMIT 0 0 D2H 65536 5','GRAPH 1 1 0 EMPTY','STATS',
                                     'SUBMIT 0 0 EMPTY','BELL','WAIT 1 10'])
        for index in (1,2,3): self.assertFalse(replies[index]['ok'])
        self.assertEqual(replies[4]['result']['write'],0)
        self.assertEqual(replies[7]['result']['state'],'complete')

    # A retained graph expands internally without inserting additional public queue packets.
    # 保留的图在内部展开，不额外插入公开队列包。
    def test_native_graph_expansion(self):
        replies = self.conversation(['GRAPH 3 0 EMPTY 1 0 EMPTY 1 1 EMPTY',
                                     'SUBMIT 0 0 GRAPH 1','BELL','WAIT 1 20','STATS'])
        self.assertEqual(len(replies[3]['result']['children']),3)
        self.assertEqual((replies[4]['result']['write'],replies[4]['result']['completed']),(1,1))


if __name__ == '__main__':
    unittest.main(verbosity=2)
