# Explicit handles preserve device ownership and buffer lifetimes in the host API.
# 显式句柄在主机接口中保留设备归属与缓冲区生命周期。
from dataclasses import dataclass
from .transport import DeviceError


@dataclass(eq=False)
class Buffer:
    device: object
    address: int
    size: int
    freed: bool = False

    # A released Python handle stays invalid even if the numeric address is reused.
    # 即使数值地址被复用，已释放的 Python 句柄也保持无效。
    def check(self):
        if self.freed or self.device.closed:
            raise DeviceError('invalid_buffer_handle')

    # Interior pointers retain a reference to their owning allocation.
    # 内部指针保留对所属分配的引用。
    def at(self, offset=0):
        self.check()
        if not isinstance(offset,int) or not 0 <= offset <= self.size:
            raise DeviceError('buffer_offset_out_of_range')
        return Pointer(self,offset)

    def free(self):
        self.device.free(self)


@dataclass(frozen=True)
class Pointer:
    buffer: Buffer
    offset: int = 0


# Resolve ranges before constructing an operation, while the backend repeats validation.
# 构造操作之前解析范围，后端仍会重复验证。
def resolve_pointer(device, value, size=0):
    pointer = value.at() if isinstance(value,Buffer) else value
    if not isinstance(pointer,Pointer) or pointer.buffer.device is not device:
        raise DeviceError('foreign_device_pointer')
    pointer.buffer.check()
    if size < 0 or pointer.offset < 0 or pointer.offset+size > pointer.buffer.size:
        raise DeviceError('copy_out_of_allocation')
    return pointer.buffer.address+pointer.offset,pointer.buffer


@dataclass(frozen=True)
class Operation:
    device: object
    fields: tuple
    buffers: tuple = ()

    # Immutable operand snapshots can be safely retained by executable graphs.
    # 不可变操作数快照可安全地由可执行图保留。
    def encode(self):
        for buffer in self.buffers:
            buffer.check()
        return ' '.join(map(str,self.fields))


# Pointer arguments also declare the resources that must remain alive until completion.
# 指针参数同时声明必须保留到完成的资源。
def kernel_arguments(device, arguments, buffers):
    words, references = [],[resolve_pointer(device,buffer)[1] for buffer in buffers]
    for argument in arguments:
        if isinstance(argument,(Buffer,Pointer)):
            address,buffer = resolve_pointer(device,argument)
            words.append(address)
            references.append(buffer)
        elif isinstance(argument,int) and 0 <= argument <= 0xFFFFFFFF:
            words.append(int(argument))
        else:
            raise DeviceError('invalid_kernel_argument')
    for buffer in references:
        resolve_pointer(device,buffer)
    return words,tuple(dict.fromkeys(references))


@dataclass(frozen=True)
class Kernel:
    device: object
    module: int

    # LDC at byte offsets 0, 4, 8, ... reads these per-launch argument words.
    # LDC 在字节偏移 0、4、8……读取这些逐次启动参数字。
    def operation(self, arguments=(), *, blocks=1, threads=32, max_steps=1000000, buffers=()):
        words,refs = kernel_arguments(self.device,arguments,buffers)
        fields = ('KERNEL',self.module,blocks,threads,max_steps,len(words),*words,
                  len(refs),*(buffer.address for buffer in refs))
        return Operation(self.device,fields,refs)

    def launch(self, arguments=(), *, stream=0, wait_for=(), **options):
        return self.device.submit(self.operation(arguments,**options),stream=stream,wait_for=wait_for)


@dataclass(frozen=True)
class Event:
    device: object
    id: int

    def query(self):
        return self.device._request(f'QUERY {self.id}')

    # Completion waits ring the doorbell but state-only queries do not advance execution.
    # 完成等待会通知 doorbell，但纯状态查询不会推进执行。
    def wait(self, max_ticks=1000000):
        self.device.flush()
        return self.device._request(f'WAIT {self.id} {max_ticks}')

    def result(self, max_ticks=1000000):
        return bytes.fromhex(self.wait(max_ticks)['data'])
