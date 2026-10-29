# Host submission is separate from the device's command processor and ISA executor.
# 主机提交与设备命令处理器及 ISA 执行器分开。
from pathlib import Path
from .transport import Connection, DeviceError
from .host_types import Buffer, Event, Kernel, Operation, resolve_pointer


class Device:
    # Start one persistent device process, keeping queue publication configurable for tests.
    # 启动一个持久设备进程，允许测试控制队列发布。
    def __init__(self, executable=None, *, sms=2, capacity=64, auto_doorbell=True, monitor_dir=None,
                 timing=False, hardware_trace=False, tensor_rtl=False, cache_rtl=False, scoreboard_rtl=False,
                 register_rtl=False, request_timeout=30):
        if hardware_trace and not timing:
            raise ValueError('hardware_trace_requires_timing')
        if tensor_rtl and not timing:
            raise ValueError('tensor_rtl_requires_timing')
        if register_rtl and not timing:
            raise ValueError('register_rtl_requires_timing')
        if scoreboard_rtl and not timing:
            raise ValueError('scoreboard_rtl_requires_timing')
        if cache_rtl and not timing:
            raise ValueError('cache_rtl_requires_timing')
        executable = executable or Path(__file__).resolve().parents[2]/'build'/'simt-device'
        self.connection = Connection(executable,sms,capacity,monitor_dir,timing=timing,
                                     hardware_trace=hardware_trace,tensor_rtl=tensor_rtl,cache_rtl=cache_rtl,
                                     scoreboard_rtl=scoreboard_rtl,register_rtl=register_rtl,request_timeout=request_timeout)
        self.auto_doorbell = auto_doorbell
        self.closed = False
        self._capture = None
        try:
            self.properties = self._request('HELLO')
        except Exception:
            self.close(wait=False)
            raise

    # Capture records submission calls and forbids incidental backend work.
    # capture 记录提交调用，并禁止顺带执行后端工作。
    def _request(self, request):
        if self._capture is not None:
            raise DeviceError('operation_during_capture')
        return self.connection.request(request)

    def allocate(self, size):
        return Buffer(self,self._request(f'ALLOC {size}'),size)

    def free(self, buffer):
        if not isinstance(buffer,Buffer):
            raise DeviceError('free_requires_allocation_base')
        resolve_pointer(self,buffer)
        self._request(f'FREE {buffer.address}')
        buffer.freed = True

    def load(self, program):
        data = program.binary() if hasattr(program,'binary') else bytes(program)
        return Kernel(self,self._request('LOAD '+(data.hex() or '-')))

    # H2D snapshots host bytes at submission or capture construction time.
    # H2D 在提交或捕获构造时保存宿主字节快照。
    def op_h2d(self, destination, data):
        data = bytes(data)
        address,buffer = resolve_pointer(self,destination,len(data))
        return Operation(self,('H2D',address,data.hex() or '-'),(buffer,))

    def op_d2h(self, source, size):
        address,buffer = resolve_pointer(self,source,size)
        return Operation(self,('D2H',address,size),(buffer,))

    def op_d2d(self, destination, source, size):
        dst,dst_buffer = resolve_pointer(self,destination,size)
        src,src_buffer = resolve_pointer(self,source,size)
        return Operation(self,('D2D',dst,src,size),(dst_buffer,src_buffer))

    def op_memset(self, destination, value, size):
        address,buffer = resolve_pointer(self,destination,size)
        return Operation(self,('FILL',address,size,value),(buffer,))

    def op_empty(self):
        return Operation(self,('EMPTY',))

    # Stream order is enforced in C++; the Python layer supplies explicit cross-stream edges.
    # stream 顺序由 C++ 保证，Python 层提供显式跨 stream 依赖边。
    def submit(self, operation, *, stream=0, wait_for=()):
        if operation.device is not self:
            raise DeviceError('foreign_device_operation')
        if self._capture is not None:
            return self._capture.record(operation,stream,wait_for)
        dependencies = []
        for event in wait_for:
            if not isinstance(event,Event) or event.device is not self:
                raise DeviceError('foreign_or_captured_event')
            dependencies.append(event.id)
        prefix = ' '.join(map(str,('SUBMIT',stream,len(dependencies),*dependencies)))
        event = Event(self,self._request(prefix+' '+operation.encode()))
        if self.auto_doorbell:
            self.flush()
        return event

    def h2d(self, destination, data, **submission):
        return self.submit(self.op_h2d(destination,data),**submission)

    def d2h(self, source, size, **submission):
        return self.submit(self.op_d2h(source,size),**submission)

    def d2d(self, destination, source, size, **submission):
        return self.submit(self.op_d2d(destination,source,size),**submission)

    def memset(self, destination, value, size, **submission):
        return self.submit(self.op_memset(destination,value,size),**submission)

    def record_event(self, stream=0):
        return self.submit(self.op_empty(),stream=stream)

    def wait_event(self, event, stream=0):
        return self.submit(self.op_empty(),stream=stream,wait_for=(event,))

    def flush(self):
        return self._request('BELL')

    def progress(self, ticks=1):
        return self._request(f'TICK {ticks}')

    def synchronize(self, max_ticks=1000000):
        self.flush()
        return self._request(f'DRAIN {max_ticks}')

    def stats(self):
        return self._request('STATS')

    def trace(self):
        return self._request('TRACE')

    def graph(self):
        from .graph import Graph
        return Graph(self)

    def capture(self, streams=(0,)):
        from .graph import Capture
        return Capture(self,streams)

    # Normal context exit drains work; exception exit explicitly aborts the owned process.
    # 正常退出上下文时排空工作，异常退出时明确中止所持有进程。
    def close(self, *, wait=True):
        if self.closed:
            return
        try:
            if wait:
                self.synchronize()
        finally:
            self._capture = None
            self.closed = True
            self.connection.close()

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        self.close(wait=kind is None)
