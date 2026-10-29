# Native nvidia-smi must query live simulator state; these tests never synthesize its output.
# 原版 nvidia-smi 必须查询存活模拟器状态，这些测试不合成其输出。
import os
from pathlib import Path
import select
import subprocess
import tempfile
import unittest
from host_support import HostCase


class NvmlTest(HostCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='simt-nvml-test-')
        self.addCleanup(self.directory.cleanup)
        self.build = Path(os.environ['SIMT_RUNNER']).resolve().parent
        self.environment = dict(os.environ, SIMT_MONITOR_DIR=self.directory.name,
                                LD_LIBRARY_PATH=str(self.build/'nvml'))
        preload = os.environ.get('SIMT_NVML_ASAN_PRELOAD')
        if preload:
            self.environment.update(LD_PRELOAD=preload, ASAN_OPTIONS='detect_leaks=0')

    def monitored(self, **options):
        return self.device(monitor_dir=self.directory.name, **options)

    # Execute the installed binary, preserving its stdout and verifying its exit status.
    # 执行已安装的原版二进制，保留其 stdout 并检查退出状态。
    def smi(self, *arguments):
        result = subprocess.run(['/usr/bin/nvidia-smi',*arguments],env=self.environment,
                                capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr+result.stdout)
        self.assertNotIn('Sanitizer',result.stderr)
        return result.stdout

    def test_native_list_default_and_live_memory(self):
        device = self.monitored(timing=True)
        self.assertIn('SIMT Lab 2 SM (simulator)',self.smi('-L'))
        buffer = device.allocate(7*1024*1024)
        device.h2d(buffer, b'SIMT').wait()
        self.assertGreater(device.stats()['hardware']['cycles'], 0)
        query = ('--query-gpu=memory.total,memory.used,memory.free','--format=csv,noheader,nounits')
        self.assertEqual(self.smi(*query).strip(),'64, 7, 57')
        table = self.smi()
        self.assertIn('NVIDIA-SMI',table)
        self.assertIn('7MiB /',table)
        self.assertIn('CUDA Version: N/A',table)
        buffer.free()
        self.assertEqual(self.smi(*query).strip(),'64, 0, 64')

    def test_multiple_sessions_and_clean_exit(self):
        first,second = self.monitored(),self.monitored(sms=4)
        listed = self.smi('-L')
        self.assertEqual(listed.count('GPU-'),2)
        self.assertIn('4 SM',listed)
        first.close()
        self.assertNotIn('2 SM',self.smi('-L'))
        second.close()
        self.assertIn('No devices found',self.smi('-L'))
        self.assertFalse(list(Path(self.directory.name).glob('*.state')))

    def test_public_abi_and_exact_allocated_bytes(self):
        device = self.monitored()
        device.allocate(12345)
        result = subprocess.run([str(self.build/'test-nvml-api'),'12345'],env=self.environment,
                                capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)

    # SIGKILL deliberately leaves registry files so the liveness lock, not cleanup, is tested.
    # SIGKILL 故意留下注册文件，以验证生命周期锁而非正常清理。
    def test_crash_rejects_stale_files_and_handles(self):
        device = self.monitored()
        process = subprocess.Popen([str(self.build/'test-nvml-api'),'0','wait'],env=self.environment,
                                   stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        self.assertTrue(select.select([process.stdout],[],[],15)[0], 'ABI probe did not start')
        self.assertEqual(process.stdout.readline().strip(),'ready')
        device.connection.process.kill()
        device.connection.process.wait(timeout=5)
        stdout,stderr = process.communicate('continue\n',timeout=10)
        self.assertEqual(process.returncode,0,stderr+stdout)
        self.assertTrue(list(Path(self.directory.name).glob('*.state')))
        self.assertIn('No devices found',self.smi('-L'))

    def test_corrupt_snapshot_is_not_a_device(self):
        device = self.monitored()
        state = next(Path(self.directory.name).glob('*.state'))
        state.write_bytes(b'bad state')
        self.assertIn('No devices found',self.smi('-L'))
        device.stats()
        self.assertIn('SIMT Lab',self.smi('-L'))

    def test_unknown_physical_properties_are_unavailable(self):
        self.monitored()
        output = self.smi('--query-gpu=temperature.gpu,power.draw,clocks.sm','--format=csv,noheader')
        self.assertIn('N/A',output)
        self.assertNotIn('0 W',output)


if __name__ == '__main__':
    unittest.main(verbosity=2)
