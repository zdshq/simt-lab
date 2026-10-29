# A persistent subprocess owns the simulated device and answers one request at a time.
# 持久子进程持有模拟设备，每次响应一个请求。
import json
import os
import math
from pathlib import Path
import select
import subprocess
import tempfile
import threading


class DeviceError(RuntimeError):
    pass


class Connection:
    # Keep stderr out of the response pipe and retain it for process-failure diagnostics.
    # 将 stderr 与响应管道分开保存，供进程失败诊断使用。
    def __init__(self, executable, sms, capacity, monitor_dir=None, *, timing=False, hardware_trace=False,
                 tensor_rtl=False, cache_rtl=False, scoreboard_rtl=False, register_rtl=False, request_timeout=30):
        self.request_timeout = float(request_timeout)
        if not math.isfinite(self.request_timeout) or self.request_timeout <= 0:
            raise ValueError('invalid_device_transport_timeout')
        self.errors = tempfile.TemporaryFile(mode='w+t')
        self.lock = threading.Lock()
        self.closed = False
        environment = dict(os.environ)
        if monitor_dir is not None:
            environment['SIMT_MONITOR_DIR'] = str(monitor_dir)
        command = [str(Path(executable).resolve()),str(sms),str(capacity)]
        if timing: command.append('--timing')
        if hardware_trace: command.append('--hardware-trace')
        if tensor_rtl: command.append('--tensor-rtl')
        if cache_rtl: command.append('--cache-rtl')
        if scoreboard_rtl: command.append('--scoreboard-rtl')
        if register_rtl: command.append('--register-rtl')
        try:
            self.process = subprocess.Popen(command,
                                            stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                            stderr=self.errors,text=True,bufsize=1,env=environment)
        except Exception:
            self.errors.close()
            raise

    # Serialize requests and bound transport waiting independently of functional tick limits.
    # 串行化请求，并独立于功能 tick 上限限制传输等待。
    def request(self, message):
        if self.closed:
            raise DeviceError('device_closed')
        with self.lock:
            try:
                self.process.stdin.write(message+'\n')
                self.process.stdin.flush()
            except (BrokenPipeError,OSError) as error:
                raise DeviceError('device_process_failed: '+self.diagnostics()) from error
            if not select.select([self.process.stdout],[],[],self.request_timeout)[0]:
                raise DeviceError('device_transport_timeout')
            line = self.process.stdout.readline()
            if not line:
                raise DeviceError('device_process_failed: '+self.diagnostics())
            response = json.loads(line)
            if not response['ok']:
                raise DeviceError(response['error'])
            return response['result']

    # Report captured sanitizer or startup errors when the process disappears.
    # 进程消失时报告捕获的 sanitizer 或启动错误。
    def diagnostics(self):
        self.errors.seek(0)
        return self.errors.read()

    # Closing the transport never pretends that unfinished work completed.
    # 关闭传输层不会将未完成工作伪装成已经完成。
    def close(self):
        if self.closed:
            return
        try:
            if self.process.poll() is None:
                self.request('QUIT')
        finally:
            self.closed = True
            self.process.stdin.close()
            self.process.stdout.close()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            self.errors.close()
