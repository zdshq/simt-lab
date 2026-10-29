# Compare merged reports with the actual native driver and independently live simulation sessions.
# 将合并报告与实际原生驱动以及独立存活的模拟会话对照。
import csv
import io
import os
from pathlib import Path
import select
import subprocess
import sys
import unittest
from test_nvml import NvmlCase

ROOT = Path(__file__).resolve().parents[1]
FIELDS = 'index,name,uuid,memory.total,driver_version,pci.bus_id'


def rows(text):
    return [[value.strip() for value in row] for row in csv.reader(io.StringIO(text))]


class MergedNvmlTest(NvmlCase):
    def setUp(self):
        super().setUp()
        self.environment['SIMT_NVML_MERGE'] = '1'
        self.baseline = rows(self.native(f'--query-gpu={FIELDS}', '--format=csv,noheader'))
        self.assertGreater(len(self.baseline), 0, 'merged NVML tests require an available native GPU')

    def native(self, *arguments):
        env = dict(os.environ, LD_LIBRARY_PATH='')
        env.pop('LD_PRELOAD', None)
        result = subprocess.run(['/usr/bin/nvidia-smi', *arguments], env=env, text=True,
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_real_metadata_and_live_simulated_memory(self):
        device = self.monitored(timing=True)
        allocation = device.allocate(7*1024*1024)
        device.h2d(allocation, b'SIMT').wait()
        report = rows(self.smi(f'--query-gpu={FIELDS}', '--format=csv,noheader'))
        self.assertEqual(report[:-1], self.baseline)
        self.assertEqual(report[-1][0], str(len(self.baseline)))
        self.assertIn('(simulator)', report[-1][1])
        selected = ['-i', report[-1][2], '--query-gpu=memory.total,memory.used,memory.free', '--format=csv,noheader,nounits']
        self.assertEqual(self.smi(*selected).strip(), '64, 7, 57')
        allocation.free()
        self.assertEqual(self.smi(*selected).strip(), '64, 0, 64')
        device.close()
        self.assertEqual(rows(self.smi(f'--query-gpu={FIELDS}', '--format=csv,noheader')), self.baseline)

    def test_uuid_index_pci_and_original_process_table(self):
        self.monitored()
        for row in self.baseline:
            for identity in (row[0], row[2]):
                selected = rows(self.smi('-i', identity, f'--query-gpu={FIELDS}', '--format=csv,noheader'))
                self.assertEqual(selected, [row])
            selected = rows(self.launcher('-i', row[5], f'--query-gpu={FIELDS}', '--format=csv,noheader'))
            self.assertEqual(selected, [row])
        # Stable native PIDs must remain in the default table, even when a simulator is also listed.
        # 即使同时列出模拟器，前后仍存活的原生 PID 也必须保留在默认表格中。
        query = ('--query-compute-apps=pid', '--format=csv,noheader')
        before = set(self.native(*query).split())
        table = self.smi()
        after = set(self.native(*query).split())
        for pid in before & after: self.assertIn(pid, table)
        self.assertIn('(simulator)', table)

    def launcher(self, *arguments):
        command = [sys.executable, str(ROOT/'tools/simt_smi.py'), '--build', str(self.build), '--merge', *arguments]
        result = subprocess.run(command, env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr+result.stdout)
        self.assertNotIn('Sanitizer', result.stderr)
        return result.stdout

    def test_multiple_simulators_keep_native_indices(self):
        first, second = self.monitored(), self.monitored(sms=4)
        listed = self.smi('-L')
        self.assertEqual(listed.count('(UUID:'), len(self.baseline)+2)
        first.close()
        listed = self.smi('-L')
        self.assertNotIn('2 SM', listed)
        self.assertIn('4 SM', listed)
        second.close()
        self.assertEqual(self.smi('-L'), self.native('-L'))

    def install_command(self, prefix, *arguments):
        return subprocess.run([sys.executable, str(ROOT/'tools/install_smi.py'), '--build', str(self.build),
                               '--prefix', str(prefix), *arguments], capture_output=True, text=True, timeout=15)

    def test_installed_command_and_reversible_removal(self):
        prefix = Path(self.directory.name)/'bin'
        self.assertEqual(self.install_command(prefix).returncode, 0)
        self.monitored()
        env = dict(os.environ, SIMT_MONITOR_DIR=self.directory.name)
        if os.environ.get('SIMT_NVML_ASAN_PRELOAD'):
            env.update(LD_PRELOAD=os.environ['SIMT_NVML_ASAN_PRELOAD'], ASAN_OPTIONS='detect_leaks=0')
        result = subprocess.run([str(prefix/'nvidia-smi'), '-L'], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count('(UUID:'), len(self.baseline)+1)
        self.assertNotIn('Sanitizer', result.stderr)
        native = subprocess.run([str(prefix/'nvidia-smi'), '--help'], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(native.stdout, self.native('--help'))
        self.assertEqual(self.install_command(prefix, '--uninstall').returncode, 0)
        self.assertFalse((prefix/'nvidia-smi').exists())

    def test_installer_preserves_unrelated_and_modified_files(self):
        prefix = Path(self.directory.name)/'bin'
        prefix.mkdir()
        target = prefix/'nvidia-smi'
        target.write_text('existing user command\n')
        self.assertNotEqual(self.install_command(prefix).returncode, 0)
        self.assertEqual(target.read_text(), 'existing user command\n')
        target.unlink()
        self.assertEqual(self.install_command(prefix).returncode, 0)
        modified = target.read_text()+'# user customization\n'
        target.write_text(modified)
        self.assertNotEqual(self.install_command(prefix, '--uninstall').returncode, 0)
        self.assertEqual(target.read_text(), modified)

    def test_abi_routing_and_reference_counts(self):
        device = self.monitored()
        device.allocate(12345)
        command = [str(self.build/'test-nvml-merge-api'), str(len(self.baseline)), '12345']
        result = subprocess.run(command, env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('Sanitizer', result.stderr)

    def test_crash_keeps_native_handles_alive(self):
        device = self.monitored()
        command = [str(self.build/'test-nvml-merge-api'), str(len(self.baseline)), '0', 'wait']
        process = subprocess.Popen(command, env=self.environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        self.assertTrue(select.select([process.stdout], [], [], 15)[0])
        self.assertEqual(process.stdout.readline().strip(), 'ready')
        device.connection.process.kill()
        device.connection.process.wait(timeout=5)
        stdout, stderr = process.communicate('continue\n', timeout=15)
        self.assertEqual(process.returncode, 0, stdout+stderr)
        self.assertNotIn('Sanitizer', stderr)
        self.assertEqual(self.smi('-L'), self.native('-L'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
